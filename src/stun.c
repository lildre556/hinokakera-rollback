#include "stun.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <ws2tcpip.h>

#include "log.h"

#define STUN_MAGIC 0x2112A442u

static const struct {
    const char *host;
    unsigned short port;
} servers[] = {
    {"stun.l.google.com", 19302},
    {"stun.cloudflare.com", 3478},
    {"stun1.l.google.com", 19302},
};
#define SERVERS (int)(sizeof servers / sizeof servers[0])

static struct sockaddr_in server_addr[SERVERS];
static bool resolved;
static struct sockaddr_in public_ep;
static uint8_t txid[12];

static void resolve(void) {
    if (resolved) {
        return;
    }
    resolved = true;
    for (int i = 0; i < SERVERS; i++) {
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        char port[8];
        snprintf(port, sizeof port, "%u", servers[i].port);
        if (getaddrinfo(servers[i].host, port, &hints, &res) == 0 && res) {
            server_addr[i] = *(struct sockaddr_in *)res->ai_addr;
            freeaddrinfo(res);
        } else {
            log_printf("stun: can't resolve %s", servers[i].host);
        }
    }
}

void stun_request(SOCKET s) {
    resolve();
    uint8_t req[20];
    req[0] = 0x00;
    req[1] = 0x01;  // Binding Request
    req[2] = req[3] = 0;
    uint32_t magic = htonl(STUN_MAGIC);
    memcpy(req + 4, &magic, 4);
    for (int i = 0; i < 12; i++) {
        txid[i] = (uint8_t)(GetTickCount() * 31 + i * 97 + (uintptr_t)&txid);
    }
    memcpy(req + 8, txid, 12);
    for (int i = 0; i < SERVERS; i++) {
        if (server_addr[i].sin_port) {
            sendto(s, (const char *)req, sizeof req, 0, (struct sockaddr *)&server_addr[i], sizeof server_addr[i]);
        }
    }
}

bool stun_handle(const uint8_t *d, int len, const struct sockaddr_in *from) {
    if (len < 20 || d[0] != 0x01 || d[1] != 0x01) {
        return false;
    }
    uint32_t magic;
    memcpy(&magic, d + 4, 4);
    if (ntohl(magic) != STUN_MAGIC) {
        return false;
    }
    int body = (d[2] << 8) | d[3];
    int p = 20;
    while (p + 4 <= 20 + body && p + 4 <= len) {
        int type = (d[p] << 8) | d[p + 1];
        int alen = (d[p + 2] << 8) | d[p + 3];
        const uint8_t *v = d + p + 4;
        if ((type == 0x0020 || type == 0x0001) && alen >= 8 && v[1] == 0x01) {
            uint16_t port = (uint16_t)((v[2] << 8) | v[3]);
            uint32_t addr;
            memcpy(&addr, v + 4, 4);
            if (type == 0x0020) {
                port ^= (uint16_t)(STUN_MAGIC >> 16);
                addr ^= htonl(STUN_MAGIC);
            }
            struct sockaddr_in ep = {0};
            ep.sin_family = AF_INET;
            ep.sin_port = htons(port);
            ep.sin_addr.s_addr = addr;
            if (ep.sin_addr.s_addr != public_ep.sin_addr.s_addr || ep.sin_port != public_ep.sin_port) {
                log_printf("stun: public address %s:%u", inet_ntoa(ep.sin_addr), port);
            }
            public_ep = ep;
            return true;
        }
        p += 4 + ((alen + 3) & ~3);
    }
    return true;
}

struct sockaddr_in stun_public(void) {
    return public_ep;
}

struct in_addr stun_lan_ip(void) {
    struct in_addr a = {0};
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = inet_addr("8.8.8.8");
    if (connect(s, (struct sockaddr *)&to, sizeof to) == 0) {
        struct sockaddr_in me;
        int n = sizeof me;
        if (getsockname(s, (struct sockaddr *)&me, &n) == 0) {
            a = me.sin_addr;
        }
    }
    closesocket(s);
    return a;
}

// ---- codes: base32 of public ip/port and LAN ip/port ----
static const char b32[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

void code_make(char *out, size_t n, struct sockaddr_in pub, struct sockaddr_in lan) {
    uint8_t raw[12];
    memcpy(raw, &pub.sin_addr.s_addr, 4);
    memcpy(raw + 4, &pub.sin_port, 2);
    memcpy(raw + 6, &lan.sin_addr.s_addr, 4);
    memcpy(raw + 10, &lan.sin_port, 2);
    char text[32];
    int bits = 0, k = 0;
    uint32_t acc = 0;
    for (int i = 0; i < 12; i++) {
        acc = (acc << 8) | raw[i];
        bits += 8;
        while (bits >= 5) {
            text[k++] = b32[(acc >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits) {
        text[k++] = b32[(acc << (5 - bits)) & 31];
    }
    text[k] = 0;
    // hino-XXXXX-XXXXX-XXXXX-XXXX
    snprintf(out, n, "hino-%.5s-%.5s-%.5s-%s", text, text + 5, text + 10, text + 15);
}

static int b32_value(char c) {
    c = (char)toupper((unsigned char)c);
    if (c == '0') {
        c = 'O';
    }
    if (c == '1') {
        c = 'I';
    }
    const char *p = strchr(b32, c);
    return p && c ? (int)(p - b32) : -1;
}

bool code_parse(const char *text, PeerCode *out) {
    memset(out, 0, sizeof *out);
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    // Plain IPv4:port?
    unsigned a, b, c, d, port;
    if (sscanf(text, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) == 5 && a < 256 && b < 256 && c < 256 && d < 256 &&
        port > 0 && port < 65536) {
        struct sockaddr_in ep = {0};
        ep.sin_family = AF_INET;
        ep.sin_port = htons((unsigned short)port);
        ep.sin_addr.s_addr = htonl((a << 24) | (b << 16) | (c << 8) | d);
        out->endpoints[0] = ep;
        out->count = 1;
        return true;
    }
    if (_strnicmp(text, "hino-", 5) != 0) {
        return false;
    }
    uint8_t raw[12];
    int bits = 0, k = 0;
    uint32_t acc = 0;
    for (const char *p = text + 5; *p && k < 12; p++) {
        if (*p == '-' || *p == ' ') {
            continue;
        }
        int v = b32_value(*p);
        if (v < 0) {
            return false;
        }
        acc = (acc << 5) | (uint32_t)v;
        bits += 5;
        if (bits >= 8) {
            raw[k++] = (uint8_t)(acc >> (bits - 8));
            bits -= 8;
        }
    }
    if (k < 12) {
        return false;
    }
    struct sockaddr_in pub = {0}, lan = {0};
    pub.sin_family = lan.sin_family = AF_INET;
    memcpy(&pub.sin_addr.s_addr, raw, 4);
    memcpy(&pub.sin_port, raw + 4, 2);
    memcpy(&lan.sin_addr.s_addr, raw + 6, 4);
    memcpy(&lan.sin_port, raw + 10, 2);
    if (pub.sin_port) {
        out->endpoints[out->count++] = pub;
    }
    if (lan.sin_port) {
        out->endpoints[out->count++] = lan;
    }
    return out->count > 0;
}
