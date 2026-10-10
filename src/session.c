#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "game.h"
#include "log.h"
#include "stun.h"

#define BUILD_ID "hinoroll-ce1"
#define PROTOCOL 1
#define DEFAULT_PORT 7000
#define PKT_MAGIC 0x4c524e48u  // "HNRL" (shared with netplay.c, which ignores these late packets)
#define PKT_HELLO 2

// The host's settings that must match (config words, see game.h): texture,
// visual/field effects (they decide which effects battle logic spawns), COM
// level, speed, win count, time, cut-in and blood. Window, FPS, volume and keys stay each player's own.
static const unsigned synced_fields[] = {
    APP_CONFIG + 5 * 4, APP_CONFIG + 6 * 4, APP_CONFIG + 7 * 4, APP_CONFIG + 10 * 4, APP_CONFIG + 11 * 4,
    APP_CONFIG + 12 * 4, APP_CONFIG + 13 * 4, APP_CONFIG + 14 * 4, APP_CONFIG + 15 * 4,
};
#define SYNCED (int)(sizeof synced_fields / sizeof synced_fields[0])

typedef struct {
    uint32_t magic, type, protocol;
    char build[32];
    uint32_t nonce, seen_nonce;
    int32_t player;
    uint32_t sent_ms, echo_ms;
    uint32_t config[16];
    char name[32];
} Hello;

typedef struct {
    SOCKET sock;
    unsigned short port;
    uint32_t nonce;
    int player;            // 1 or 2 (ours)
    PeerCode candidates;
    bool have_peer;
    struct sockaddr_in peer;
    Hello their;
    uint32_t their_seen_at;
    int rtt;
    bool done;
    char error[160];
    char build[32];
    uint32_t config[16];
    DWORD last_hello;
} Conn;

static bool pending;
static NetplaySetup pending_setup;

static char ini_path[MAX_PATH];

static void ini_init(void) {
    snprintf(ini_path, sizeof ini_path, "%s\\hinoroll.ini", game_dir());
}

static int ini_int(const char *key, int def) {
    return (int)GetPrivateProfileIntA("netplay", key, def, ini_path);
}

static void ini_get(const char *key, char *out, int n) {
    GetPrivateProfileStringA("netplay", key, "", out, n, ini_path);
}

static void ini_set(const char *key, const char *value) {
    WritePrivateProfileStringA("netplay", key, value, ini_path);
}

// Same game data on both sides: a quick hash of the data files that hold the
// characters' moves and the executable's size.
static void make_build_id(char *out, size_t n) {
    static const char *files[] = {"vsdat.dat", "vsobj2.dat", "nvdat.dat", "hinokakera.exe"};
    uint32_t h = 2166136261u;
    static uint8_t buf[1 << 16];
    for (int i = 0; i < 4; i++) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\%s", game_dir(), files[i]);
        FILE *f = fopen(path, "rb");
        if (!f) {
            continue;
        }
        size_t got;
        while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
            for (size_t k = 0; k < got; k++) {
                h = (h ^ buf[k]) * 16777619u;
            }
        }
        fclose(f);
    }
    snprintf(out, n, "%s-%08x", BUILD_ID, h);
}

static bool open_socket(Conn *c, unsigned short port) {
    c->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (c->sock == INVALID_SOCKET) {
        snprintf(c->error, sizeof c->error, "can't create a UDP socket (%d)", WSAGetLastError());
        return false;
    }
    u_long nb = 1;
    ioctlsocket(c->sock, FIONBIO, &nb);
    for (unsigned short p = port; p < port + 20; p++) {
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_port = htons(p);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(c->sock, (struct sockaddr *)&a, sizeof a) == 0) {
            c->port = p;
            log_printf("session: UDP port %u", p);
            return true;
        }
    }
    snprintf(c->error, sizeof c->error, "UDP ports %u-%u are all in use", port, port + 19);
    closesocket(c->sock);
    c->sock = INVALID_SOCKET;
    return false;
}

static void send_hello(Conn *c, const struct sockaddr_in *to) {
    Hello h = {0};
    h.magic = PKT_MAGIC;
    h.type = PKT_HELLO;
    h.protocol = PROTOCOL;
    snprintf(h.build, sizeof h.build, "%s", c->build);
    h.nonce = c->nonce;
    h.seen_nonce = c->their.nonce;
    h.player = c->player;
    h.sent_ms = GetTickCount();
    h.echo_ms = c->their.sent_ms ? c->their.sent_ms + (GetTickCount() - c->their_seen_at) : 0;
    memcpy(h.config, c->config, sizeof h.config);
    DWORD n = sizeof h.name;
    GetComputerNameA(h.name, &n);
    sendto(c->sock, (const char *)&h, sizeof h, 0, (const struct sockaddr *)to, sizeof *to);
}

// Handles one packet. Returns true when it was ours.
static void handle_packet(Conn *c, const uint8_t *d, int len, const struct sockaddr_in *from) {
    if (stun_handle(d, len, from)) {
        return;
    }
    if (len < (int)sizeof(Hello) || *(const uint32_t *)d != PKT_MAGIC || ((const Hello *)d)->type != PKT_HELLO) {
        return;
    }
    const Hello *h = (const Hello *)d;
    if (h->nonce == c->nonce) {
        return;  // our own (looped back)
    }
    if (h->protocol != PROTOCOL || strncmp(h->build, c->build, sizeof h->build) != 0) {
        snprintf(c->error, sizeof c->error, "the opponent's game differs (%.31s, here %.31s): use the same mod and game files",
                 h->build, c->build);
        return;
    }
    if (h->player == c->player) {
        snprintf(c->error, sizeof c->error, "you both picked Player %d: one of you must pick the other", c->player);
        return;
    }
    c->error[0] = 0;
    if (!c->have_peer) {
        c->have_peer = true;
        c->peer = *from;
        log_printf("session: hello from %s:%u (%.31s)", inet_ntoa(from->sin_addr), ntohs(from->sin_port), h->name);
    }
    c->their = *h;
    c->their_seen_at = GetTickCount();
    if (h->echo_ms) {
        int rtt = (int)(GetTickCount() - h->echo_ms);
        if (rtt >= 0 && rtt < 5000) {
            c->rtt = c->rtt ? (c->rtt * 3 + rtt) / 4 : rtt;
        }
    }
    if (h->seen_nonce == c->nonce) {
        c->done = true;
    }
}

static void poll_socket(Conn *c) {
    uint8_t buf[1500];
    for (;;) {
        struct sockaddr_in from;
        int fl = sizeof from;
        int n = recvfrom(c->sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) {
            break;
        }
        handle_packet(c, buf, n, &from);
    }
}

static void say_hello(Conn *c) {
    DWORD now = GetTickCount();
    if (now - c->last_hello < 100) {
        return;
    }
    c->last_hello = now;
    if (c->have_peer) {
        send_hello(c, &c->peer);
    } else {
        for (int i = 0; i < c->candidates.count; i++) {
            send_hello(c, &c->candidates.endpoints[i]);
        }
    }
}

static int auto_delay(int rtt) {
    // Cover about half the one-way trip with delay, the rest with rollback.
    int frames = (rtt / 2 + 16) / 17;
    int d = frames - 1;
    return d < 1 ? 1 : d > 4 ? 4 : d;
}

static void apply_host_config(void *app, const uint32_t *config) {
    for (int i = 0; i < SYNCED; i++) {
        APP_U32(app, synced_fields[i]) = config[i];
    }
}

static void finish(Conn *c, void *app, int delay_setting) {
    if (c->player == 2) {
        apply_host_config(app, c->their.config);
    }
    // Keep answering a moment so the opponent hears our last hello too.
    DWORD until = GetTickCount() + 800;
    while (GetTickCount() < until) {
        poll_socket(c);
        say_hello(c);
        Sleep(10);
    }
    pending = true;
    pending_setup.local_player = c->player;
    pending_setup.sock = (uintptr_t)c->sock;
    pending_setup.peer_ip = c->peer.sin_addr.s_addr;
    pending_setup.peer_port = ntohs(c->peer.sin_port);
    pending_setup.delay = delay_setting >= 0 ? delay_setting : auto_delay(c->rtt);
    snprintf(pending_setup.peer_name, sizeof pending_setup.peer_name, "%.31s", c->their.name);
    log_printf("session: connected as player %d to %s:%u (%s), rtt %d ms, delay %d", c->player,
               inet_ntoa(c->peer.sin_addr), ntohs(c->peer.sin_port), c->their.name, c->rtt, pending_setup.delay);
}

static void conn_init(Conn *c, void *app, int player) {
    memset(c, 0, sizeof *c);
    c->sock = INVALID_SOCKET;
    c->player = player;
    c->nonce = (GetTickCount() * 2654435761u) ^ GetCurrentProcessId() ^ (uint32_t)(uintptr_t)c;
    if (c->nonce == 0) {
        c->nonce = 1;
    }
    make_build_id(c->build, sizeof c->build);
    for (int i = 0; i < SYNCED; i++) {
        c->config[i] = APP_U32(app, synced_fields[i]);
    }
}

// ---- --netplay-direct P PORT IP:PORT [DELAY] ----
static bool direct(void *app, const char *args) {
    int player = 0, port = 0, delay = -1;
    char peer[64] = {0};
    if (sscanf(args, "%d %d %63s %d", &player, &port, peer, &delay) < 3 || (player != 1 && player != 2)) {
        log_printf("session: bad --netplay-direct arguments '%s'", args);
        return false;
    }
    Conn c;
    conn_init(&c, app, player);
    if (!open_socket(&c, (unsigned short)port) || !code_parse(peer, &c.candidates)) {
        log_printf("session: %s", c.error[0] ? c.error : "bad peer address");
        return false;
    }
    DWORD give_up = GetTickCount() + 60000;
    while (!c.done && GetTickCount() < give_up) {
        poll_socket(&c);
        say_hello(&c);
        Sleep(5);
    }
    if (!c.done) {
        log_printf("session: no answer from %s (%s)", peer, c.error);
        closesocket(c.sock);
        return false;
    }
    finish(&c, app, delay);
    return true;
}

// ---- the connection window ----
enum {
    ID_MYCODE = 101, ID_COPY, ID_PEERCODE, ID_P1, ID_P2, ID_DELAY, ID_CONNECT, ID_OFFLINE, ID_STATUS, ID_PASTE
};

typedef struct {
    Conn conn;
    void *app;
    HWND wnd;
    bool connecting, closed, offline;
    char last_code[64];
    DWORD last_stun;
    int delay_setting;
} Dialog;
static Dialog *dlg;

static void set_text(int id, const char *text) {
    char cur[512];
    GetDlgItemTextA(dlg->wnd, id, cur, sizeof cur);
    if (strcmp(cur, text) != 0) {
        SetDlgItemTextA(dlg->wnd, id, text);
    }
}

static void copy_text(HWND w, const char *text) {
    if (!OpenClipboard(w)) {
        return;
    }
    EmptyClipboard();
    size_t n = strlen(text) + 1;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    memcpy(GlobalLock(g), text, n);
    GlobalUnlock(g);
    SetClipboardData(CF_TEXT, g);
    CloseClipboard();
}

static void paste_text(HWND w, int id) {
    if (!OpenClipboard(w)) {
        return;
    }
    HANDLE g = GetClipboardData(CF_TEXT);
    if (g) {
        const char *t = GlobalLock(g);
        if (t) {
            char buf[256];
            snprintf(buf, sizeof buf, "%s", t);
            for (char *p = buf; *p; p++) {
                if (*p == '\r' || *p == '\n') {
                    *p = 0;
                    break;
                }
            }
            SetDlgItemTextA(w, id, buf);
            GlobalUnlock(g);
        }
    }
    CloseClipboard();
}

static void start_connect(void) {
    char text[256];
    GetDlgItemTextA(dlg->wnd, ID_PEERCODE, text, sizeof text);
    Conn *c = &dlg->conn;
    if (!code_parse(text, &c->candidates)) {
        set_text(ID_STATUS, "That doesn't look like a code (hino-...) or an IP:port.");
        return;
    }
    c->player = IsDlgButtonChecked(dlg->wnd, ID_P2) == BST_CHECKED ? 2 : 1;
    int sel = (int)SendDlgItemMessageA(dlg->wnd, ID_DELAY, CB_GETCURSEL, 0, 0);
    dlg->delay_setting = sel <= 0 ? -1 : sel - 1;
    c->have_peer = false;
    c->done = false;
    c->error[0] = 0;
    memset(&c->their, 0, sizeof c->their);
    dlg->connecting = true;
    ini_set("peer", text);
    ini_set("player", c->player == 2 ? "2" : "1");
    char d[8];
    snprintf(d, sizeof d, "%d", dlg->delay_setting);
    ini_set("delay", d);
    SetDlgItemTextA(dlg->wnd, ID_CONNECT, "Cancel");
}

static LRESULT CALLBACK dlg_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_COPY: {
            char code[128];
            GetDlgItemTextA(w, ID_MYCODE, code, sizeof code);
            copy_text(w, code);
            set_text(ID_STATUS, "Your code is on the clipboard. Send it to your opponent.");
            return 0;
        }
        case ID_PASTE:
            paste_text(w, ID_PEERCODE);
            return 0;
        case ID_CONNECT:
            if (dlg->connecting) {
                dlg->connecting = false;
                SetDlgItemTextA(w, ID_CONNECT, "Connect");
                set_text(ID_STATUS, "Cancelled.");
            } else {
                start_connect();
            }
            return 0;
        case ID_OFFLINE:
            dlg->offline = true;
            return 0;
        }
        break;
    case WM_CLOSE:
        dlg->offline = true;
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

static HWND add(const char *cls, const char *text, DWORD style, int x, int y, int cx, int cy, int id) {
    HWND h = CreateWindowExA(strcmp(cls, "EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y,
                             cx, cy, dlg->wnd, (HMENU)(intptr_t)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return h;
}

static void update_dialog(void) {
    Conn *c = &dlg->conn;
    DWORD now = GetTickCount();
    // Keep the NAT mapping fresh and the code current.
    if (now - dlg->last_stun > (stun_public().sin_port ? 10000u : 700u)) {
        dlg->last_stun = now;
        stun_request(c->sock);
    }
    struct sockaddr_in pub = stun_public(), lan = {0};
    lan.sin_family = AF_INET;
    lan.sin_addr = stun_lan_ip();
    lan.sin_port = htons(c->port);
    char code[64];
    if (pub.sin_port) {
        code_make(code, sizeof code, pub, lan);
    } else {
        struct sockaddr_in none = {0};
        code_make(code, sizeof code, none, lan);
    }
    if (strcmp(code, dlg->last_code) != 0) {
        snprintf(dlg->last_code, sizeof dlg->last_code, "%s", code);
        set_text(ID_MYCODE, code);
    }
    char status[256];
    if (dlg->connecting) {
        say_hello(c);
        if (c->error[0]) {
            snprintf(status, sizeof status, "%s", c->error);
        } else if (c->have_peer) {
            snprintf(status, sizeof status, "Found %s, finishing the handshake...", c->their.name);
        } else {
            snprintf(status, sizeof status, "Connecting... (both of you press Connect with each other's code)");
        }
    } else if (!pub.sin_port) {
        snprintf(status, sizeof status, "Finding your internet address... (LAN play works already)");
    } else {
        snprintf(status, sizeof status, "UDP port %u. Send your code, paste your opponent's, then Connect.", c->port);
    }
    if (!dlg->connecting || c->error[0] || c->have_peer || strncmp(status, "Connecting", 10) == 0) {
        char cur[256];
        GetDlgItemTextA(dlg->wnd, ID_STATUS, cur, sizeof cur);
        if (strncmp(cur, "Your code is on", 15) != 0 || dlg->connecting) {
            set_text(ID_STATUS, status);
        }
    }
}

static bool dialog(void *app) {
    static Dialog d;
    memset(&d, 0, sizeof d);
    dlg = &d;
    d.app = app;
    conn_init(&d.conn, app, 1);
    if (!open_socket(&d.conn, (unsigned short)ini_int("port", DEFAULT_PORT))) {
        MessageBoxA(NULL, d.conn.error, "Hinokakera netplay", MB_OK | MB_ICONERROR);
        return false;
    }
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = dlg_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "HinorollNetplay";
    RegisterClassA(&wc);
    RECT r = {0, 0, 520, 270};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
#ifdef HINOROLL_HARNESS
    DWORD ex = 0;  // test builds stay out of the way (tools/dev/dialog_connect.py drives the window)
#else
    DWORD ex = WS_EX_TOPMOST;
#endif
    d.wnd = CreateWindowExA(ex, "HinorollNetplay", "Hinokakera netplay (GGPO rollback)",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                            r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    add("STATIC", "Your code - send it to your opponent:", 0, 12, 12, 480, 18, 0);
    add("EDIT", "", ES_READONLY | ES_AUTOHSCROLL, 12, 32, 390, 24, ID_MYCODE);
    add("BUTTON", "Copy", BS_PUSHBUTTON, 410, 32, 96, 24, ID_COPY);
    add("STATIC", "Your opponent's code (or IP:port):", 0, 12, 66, 480, 18, 0);
    add("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, 12, 86, 390, 24, ID_PEERCODE);
    add("BUTTON", "Paste", BS_PUSHBUTTON, 410, 86, 96, 24, ID_PASTE);
    add("BUTTON", "Player 1 (my game settings)", BS_AUTORADIOBUTTON | WS_GROUP, 12, 122, 200, 20, ID_P1);
    add("BUTTON", "Player 2", BS_AUTORADIOBUTTON, 230, 122, 120, 20, ID_P2);
    add("STATIC", "Input delay:", 0, 12, 156, 80, 18, 0);
    HWND combo = add("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 92, 152, 100, 160, ID_DELAY);
    const char *delays[] = {"Auto", "0 frames", "1 frame", "2 frames", "3 frames", "4 frames"};
    for (int i = 0; i < 6; i++) {
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)delays[i]);
    }
    add("BUTTON", "Connect", BS_DEFPUSHBUTTON | WS_TABSTOP, 12, 190, 130, 30, ID_CONNECT);
    add("BUTTON", "Play offline", BS_PUSHBUTTON, 150, 190, 130, 30, ID_OFFLINE);
    add("STATIC", "", 0, 12, 232, 496, 34, ID_STATUS);
    // Last time's choices.
    char peer[256];
    ini_get("peer", peer, sizeof peer);
    SetDlgItemTextA(d.wnd, ID_PEERCODE, peer);
    CheckRadioButton(d.wnd, ID_P1, ID_P2, ini_int("player", 1) == 2 ? ID_P2 : ID_P1);
    int delay = ini_int("delay", -1);
    SendMessageA(combo, CB_SETCURSEL, delay < 0 || delay > 4 ? 0 : delay + 1, 0);
#ifdef HINOROLL_HARNESS
    ShowWindow(d.wnd, SW_SHOWNOACTIVATE);
    SetWindowPos(d.wnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
    ShowWindow(d.wnd, SW_SHOW);
    SetForegroundWindow(d.wnd);
#endif

    while (!d.offline) {
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            if (!IsDialogMessageA(d.wnd, &m)) {
                TranslateMessage(&m);
                DispatchMessageA(&m);
            }
        }
        poll_socket(&d.conn);
        update_dialog();
        if (d.connecting && d.conn.done) {
            set_text(ID_STATUS, "Connected! Starting...");
            break;
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 10, QS_ALLINPUT);
    }
    DestroyWindow(d.wnd);
    if (d.offline || !d.conn.done) {
        closesocket(d.conn.sock);
        log_printf("session: playing offline");
        return false;
    }
    finish(&d.conn, app, d.delay_setting);
    return true;
}

bool session_before_init(void *app) {
    ini_init();
    const char *cmd = GetCommandLineA();
    const char *direct_args = strstr(cmd, "--netplay-direct");
    bool want_dialog = strstr(cmd, "--netplay") != NULL;
    if (!direct_args && !want_dialog) {
        return false;
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    if (direct_args) {
        return direct(app, direct_args + strlen("--netplay-direct"));
    }
    return dialog(app);
}

bool session_take(NetplaySetup *out) {
    if (!pending) {
        return false;
    }
    pending = false;
    *out = pending_setup;
    return true;
}
