#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <winsock2.h>

// Public address discovery (STUN binding requests on the game's own UDP
// socket, so the mapping it reports is the one the opponent must send to) and
// the connection codes players exchange.

// Starts asking the STUN servers (resolves them on first use). Call stun_poll
// with packets that arrive on the socket; it returns true for STUN answers.
void stun_request(SOCKET s);
bool stun_handle(const uint8_t *data, int len, const struct sockaddr_in *from);
// The public endpoint once known (sin_port 0 until then).
struct sockaddr_in stun_public(void);
// This machine's LAN address (the interface that routes to the internet).
struct in_addr stun_lan_ip(void);

typedef struct {
    struct sockaddr_in endpoints[2];  // public, LAN
    int count;
} PeerCode;

// "hino-XXXX..." from the public and LAN endpoints.
void code_make(char *out, size_t n, struct sockaddr_in pub, struct sockaddr_in lan);
// Accepts a code or a plain "a.b.c.d:port".
bool code_parse(const char *text, PeerCode *out);
