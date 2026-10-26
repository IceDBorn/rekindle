// WinHTTP hooks: log the game's requests at debug level, send them to the local server in local mode, and
// watch them for the run marker.
#include "rekindle.h"
#include <winhttp.h>
#include <stdio.h>
#include <string.h>

static void wide(char *dst, size_t cap, const wchar_t *src, int len)
{
    dst[0] = 0;
    if (!src)
        return;
    int n = WideCharToMultiByte(CP_UTF8, 0, src, len, dst, (int)cap - 1, NULL, NULL);
    dst[n > 0 ? n : 0] = 0;
}

typedef HINTERNET(WINAPI *Open_t)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef HINTERNET(WINAPI *Connect_t)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET(WINAPI *OpenRequest_t)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
typedef BOOL(WINAPI *AddHeaders_t)(HINTERNET, LPCWSTR, DWORD, DWORD);
typedef BOOL(WINAPI *Send_t)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL(WINAPI *Receive_t)(HINTERNET, LPVOID);
typedef BOOL(WINAPI *QueryHeaders_t)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
typedef BOOL(WINAPI *Read_t)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL(WINAPI *Close_t)(HINTERNET);
typedef WINHTTP_STATUS_CALLBACK(WINAPI *SetCallback_t)(HINTERNET, WINHTTP_STATUS_CALLBACK, DWORD, DWORD_PTR);

static Open_t real_open;
static Connect_t real_connect;
static OpenRequest_t real_open_request;
static AddHeaders_t real_add_headers;
static Send_t real_send;
static Receive_t real_receive;
static QueryHeaders_t real_query_headers;
static Read_t real_read;
static Close_t real_close;
static SetCallback_t real_set_callback;
static WINHTTP_STATUS_CALLBACK game_callback;

static HINTERNET WINAPI hook_open(LPCWSTR agent, DWORD access, LPCWSTR proxy, LPCWSTR bypass, DWORD flags)
{
    HINTERNET h = real_open(agent, access, proxy, bypass, flags);
    char a[256], info[320];
    wide(a, sizeof a, agent, -1);
    snprintf(info, sizeof info, "agent=\"%s\" flags=0x%lx", a, flags);
    log_event("Open", h, info, NULL, 0);
    return h;
}

static HINTERNET WINAPI hook_connect(HINTERNET s, LPCWSTR server, INTERNET_PORT port, DWORD r)
{
    char host[256], info[320];
    wide(host, sizeof host, server, -1);
    // No real host is ever contacted in local mode, even when the server failed to start.
    bool local = local_mode();
    HINTERNET h = local ? real_connect(s, L"127.0.0.1", (INTERNET_PORT)local_port, r) : real_connect(s, server, port, r);
    snprintf(info, sizeof info, "session=%p host=%s port=%u%s", s, host, port, local ? " local" : "");
    log_event("Connect", h, info, NULL, 0);
    return h;
}

static HINTERNET WINAPI hook_open_request(HINTERNET c, LPCWSTR verb, LPCWSTR object, LPCWSTR version,
                                          LPCWSTR referrer, LPCWSTR *accept, DWORD flags)
{
    if (local_mode())
        flags &= ~WINHTTP_FLAG_SECURE;
    HINTERNET h = real_open_request(c, verb, object, version, referrer, accept, flags);
    char v[32], obj[1024], info[1200];
    wide(v, sizeof v, verb ? verb : L"GET", -1);
    wide(obj, sizeof obj, object, -1);
    snprintf(info, sizeof info, "conn=%p %s %s flags=0x%lx", c, v, obj, flags);
    log_event("OpenRequest", h, info, NULL, 0);
    return h;
}

static BOOL WINAPI hook_add_headers(HINTERNET h, LPCWSTR headers, DWORD len, DWORD mods)
{
    char buf[4096];
    wide(buf, sizeof buf, headers, len == (DWORD)-1 ? -1 : (int)len);
    log_event("AddHeaders", h, NULL, buf, strlen(buf));
    return real_add_headers(h, headers, len, mods);
}

static BOOL WINAPI hook_send(HINTERNET h, LPCWSTR headers, DWORD hlen, LPVOID body, DWORD blen, DWORD total,
                             DWORD_PTR ctx)
{
    char buf[4096], info[64];
    wide(buf, sizeof buf, headers, hlen == (DWORD)-1 ? -1 : (int)hlen);
    if (buf[0])
        log_event("SendHeaders", h, NULL, buf, strlen(buf));
    snprintf(info, sizeof info, "body=%lu total=%lu", blen, total);
    log_event("Send", h, info, body, blen);
    run_marker_http(body, blen);
    BOOL ok = real_send(h, headers, hlen, body, blen, total, ctx);
    if (!ok) {
        snprintf(info, sizeof info, "error=%lu", GetLastError());
        log_event("SendFailed", h, info, NULL, 0);
    }
    return ok;
}

static BOOL WINAPI hook_receive(HINTERNET h, LPVOID r)
{
    BOOL ok = real_receive(h, r);
    char info[64];
    snprintf(info, sizeof info, "ok=%d error=%lu", ok, ok ? 0 : GetLastError());
    log_event("Receive", h, info, NULL, 0);
    return ok;
}

static BOOL WINAPI hook_query_headers(HINTERNET h, DWORD level, LPCWSTR name, LPVOID buf, LPDWORD len, LPDWORD idx)
{
    BOOL ok = real_query_headers(h, level, name, buf, len, idx);
    if (ok && buf && len) {
        char info[64], text[8192];
        snprintf(info, sizeof info, "level=0x%lx", level);
        if (level & WINHTTP_QUERY_FLAG_NUMBER)
            snprintf(text, sizeof text, "%lu", *(DWORD *)buf);
        else
            wide(text, sizeof text, buf, (int)(*len / 2));
        log_event("Headers", h, info, text, strlen(text));
    }
    return ok;
}

static BOOL WINAPI hook_read(HINTERNET h, LPVOID buf, DWORD n, LPDWORD got)
{
    BOOL ok = real_read(h, buf, n, got);
    char info[64];
    if (got) {
        snprintf(info, sizeof info, "ok=%d bytes=%lu", ok, ok ? *got : 0);
        log_event("Read", h, info, buf, ok ? *got : 0);
        if (ok)
            run_marker_http(buf, *got);
    } else {
        snprintf(info, sizeof info, "async ok=%d cap=%lu", ok, n);
        log_event("ReadAsync", h, info, NULL, 0);
    }
    return ok;
}

static BOOL WINAPI hook_close(HINTERNET h)
{
    log_event("Close", h, NULL, NULL, 0);
    return real_close(h);
}

static void CALLBACK wrap_callback(HINTERNET h, DWORD_PTR ctx, DWORD status, LPVOID info, DWORD len)
{
    char text[96];
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        snprintf(text, sizeof text, "READ_COMPLETE bytes=%lu", len);
        log_event("Callback", h, text, info, len);
        run_marker_http(info, len);
        break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
        WINHTTP_ASYNC_RESULT *r = info;
        snprintf(text, sizeof text, "REQUEST_ERROR api=%llu error=%lu", (unsigned long long)r->dwResult, r->dwError);
        log_event("Callback", h, text, NULL, 0);
        break;
    }
    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
    case WINHTTP_CALLBACK_STATUS_HANDLE_CREATED:
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
        break;
    default:
        snprintf(text, sizeof text, "status=0x%lx len=%lu", status, len);
        log_event("Callback", h, text, NULL, 0);
    }
    game_callback(h, ctx, status, info, len);
}

static WINHTTP_STATUS_CALLBACK WINAPI hook_set_callback(HINTERNET h, WINHTTP_STATUS_CALLBACK cb, DWORD flags,
                                                        DWORD_PTR r)
{
    char info[96];
    snprintf(info, sizeof info, "cb=%p flags=0x%lx", (void *)cb, flags);
    log_event("SetCallback", h, info, NULL, 0);
    // Only one game callback is wrapped; any other passes through untouched.
    if (cb && cb != WINHTTP_INVALID_STATUS_CALLBACK && (!game_callback || game_callback == cb)) {
        game_callback = cb;
        WINHTTP_STATUS_CALLBACK prev = real_set_callback(h, wrap_callback, flags, r);
        return prev == wrap_callback ? game_callback : prev;
    }
    return real_set_callback(h, cb, flags, r);
}

const struct import_hook http_imports[] = {
    {"WinHttpOpen", hook_open, (void **)&real_open},
    {"WinHttpConnect", hook_connect, (void **)&real_connect},
    {"WinHttpOpenRequest", hook_open_request, (void **)&real_open_request},
    {"WinHttpAddRequestHeaders", hook_add_headers, (void **)&real_add_headers},
    {"WinHttpSendRequest", hook_send, (void **)&real_send},
    {"WinHttpReceiveResponse", hook_receive, (void **)&real_receive},
    {"WinHttpQueryHeaders", hook_query_headers, (void **)&real_query_headers},
    {"WinHttpReadData", hook_read, (void **)&real_read},
    {"WinHttpCloseHandle", hook_close, (void **)&real_close},
    {"WinHttpSetStatusCallback", hook_set_callback, (void **)&real_set_callback},
};
const int http_import_count = sizeof http_imports / sizeof *http_imports;
