#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

static char dir[MAX_PATH];
static HANDLE file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION lock;

void log_init(void) {
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    char *slash = strrchr(dir, '\\');
    if (slash) {
        *slash = 0;
    }
    InitializeCriticalSection(&lock);
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\hinoroll.log", dir);
    file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

const char *game_dir(void) {
    return dir;
}

void log_printf(const char *fmt, ...) {
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    char buf[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof buf - n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) {
        m = 0;
    }
    n += m;
    if (n > (int)sizeof buf - 2) {
        n = sizeof buf - 2;
    }
    buf[n++] = '\r';
    buf[n++] = '\n';
    EnterCriticalSection(&lock);
    DWORD written;
    WriteFile(file, buf, n, &written, NULL);
    LeaveCriticalSection(&lock);
}
