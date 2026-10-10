/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include "types.h"
#include "udp.h"

// hinoroll: a socket handed in by the mod (already bound, NAT mapping punched),
// a filter for the mod's own packets on it, and the socket in use.
static SOCKET hinoroll_next_socket = INVALID_SOCKET;
static SOCKET hinoroll_current_socket = INVALID_SOCKET;
static int (__cdecl *hinoroll_raw_handler)(const void *, const unsigned char *, int) = NULL;

void __cdecl ggpo_hinoroll_use_socket(unsigned int sock) { hinoroll_next_socket = (SOCKET)sock; }
void __cdecl ggpo_hinoroll_set_raw_handler(int (__cdecl *fn)(const void *, const unsigned char *, int)) { hinoroll_raw_handler = fn; }
unsigned int __cdecl ggpo_hinoroll_socket(void) { return (unsigned int)hinoroll_current_socket; }

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

SOCKET
CreateSocket(uint16 bind_port, int retries)
{
   SOCKET s;
   sockaddr_in sin;
   uint16 port;
   int optval = 1;

   s = socket(AF_INET, SOCK_DGRAM, 0);
   setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&optval, sizeof optval);
   setsockopt(s, SOL_SOCKET, SO_DONTLINGER, (const char *)&optval, sizeof optval);

   // non-blocking...
   u_long iMode = 1;
   ioctlsocket(s, FIONBIO, &iMode);

   sin.sin_family = AF_INET;
   sin.sin_addr.s_addr = htonl(INADDR_ANY);
   for (port = bind_port; port <= bind_port + retries; port++) {
      sin.sin_port = htons(port);
      if (bind(s, (sockaddr *)&sin, sizeof sin) != SOCKET_ERROR) {
         Log("Udp bound to port: %d.\n", port);
         return s;
      }
   }
   closesocket(s);
   return INVALID_SOCKET;
}

Udp::Udp() :
   _socket(INVALID_SOCKET),
   _callbacks(NULL)
{
}

Udp::~Udp(void)
{
   if (_socket != INVALID_SOCKET) {
      closesocket(_socket);
      if (hinoroll_current_socket == _socket) {
         hinoroll_current_socket = INVALID_SOCKET;
      }
      _socket = INVALID_SOCKET;
   }
}

void
Udp::Init(uint16 port, Poll *poll, Callbacks *callbacks)
{
   _callbacks = callbacks;

   _poll = poll;
   _poll->RegisterLoop(this);

   if (hinoroll_next_socket != INVALID_SOCKET) {
      // hinoroll: use the mod's socket (already bound, NAT mapping punched).
      _socket = hinoroll_next_socket;
      hinoroll_next_socket = INVALID_SOCKET;
      u_long iMode = 1;
      ioctlsocket(_socket, FIONBIO, &iMode);
      Log("using the socket handed in by the mod.\n");
   } else {
      Log("binding udp socket to port %d.\n", port);
      _socket = CreateSocket(port, 0);
   }
   // hinoroll: an ICMP "port unreachable" must not turn into WSAECONNRESET on later reads.
   BOOL report = FALSE;
   DWORD bytes = 0;
   WSAIoctl(_socket, SIO_UDP_CONNRESET, &report, sizeof report, NULL, 0, &bytes, NULL, NULL);
   hinoroll_current_socket = _socket;
}

void
Udp::SendTo(char *buffer, int len, int flags, struct sockaddr *dst, int destlen)
{
   struct sockaddr_in *to = (struct sockaddr_in *)dst;

   int res = sendto(_socket, buffer, len, flags, dst, destlen);
   if (res == SOCKET_ERROR) {
      DWORD err = WSAGetLastError();
      Log("unknown error in sendto (erro: %d  wsaerr: %d).\n", res, err);
      return; // hinoroll: a lost packet is fine (GGPO resends); don't kill the game
   }
   char dst_ip[1024];
   Log("sent packet length %d to %s:%d (ret:%d).\n", len, inet_ntop(AF_INET, (void *)&to->sin_addr, dst_ip, ARRAY_SIZE(dst_ip)), ntohs(to->sin_port), res);
}

bool
Udp::OnLoopPoll(void *cookie)
{
   uint8          recv_buf[MAX_UDP_PACKET_SIZE];
   sockaddr_in    recv_addr;
   int            recv_addr_len;

   for (;;) {
      recv_addr_len = sizeof(recv_addr);
      int len = recvfrom(_socket, (char *)recv_buf, MAX_UDP_PACKET_SIZE, 0, (struct sockaddr *)&recv_addr, &recv_addr_len);

      // TODO: handle len == 0... indicates a disconnect.

      if (len == -1) {
         int error = WSAGetLastError();
         if (error != WSAEWOULDBLOCK) {
            Log("recvfrom WSAGetLastError returned %d (%x).\n", error, error);
         }
         break;
      } else if (len > 0) {
         char src_ip[1024];
         Log("recvfrom returned (len:%d  from:%s:%d).\n", len, inet_ntop(AF_INET, (void*)&recv_addr.sin_addr, src_ip, ARRAY_SIZE(src_ip)), ntohs(recv_addr.sin_port) );
         if (hinoroll_raw_handler && hinoroll_raw_handler(&recv_addr, recv_buf, len)) {
            continue; // hinoroll: one of the mod's own packets
         }
         UdpMsg *msg = (UdpMsg *)recv_buf;
         _callbacks->OnMsg(recv_addr, msg, len);
      } 
   }
   return true;
}


void
Udp::Log(const char *fmt, ...)
{
   char buf[1024];
   size_t offset;
   va_list args;

   strcpy_s(buf, "udp | ");
   offset = strlen(buf);
   va_start(args, fmt);
   vsnprintf(buf + offset, ARRAY_SIZE(buf) - offset - 1, fmt, args);
   buf[ARRAY_SIZE(buf)-1] = '\0';
   ::Log(buf);
   va_end(args);
}
