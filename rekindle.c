// Rekindle: restores crashed runs. At log level debug it also logs the game's WinHTTP and Steam calls.
// Hooks nightreign.exe's import table, so it sees requests before cl_server_redirector rewrites them.
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>

static void run_marker_http(const void *data, size_t n);
static void note_mate(uint64_t id);
static bool local_mode(void);
static int cfg(const char *section, const char *key, int def);
static volatile LONG local_port;
static bool run_restored;

static CRITICAL_SECTION lock;
static uint64_t t0;

static uint64_t unix_ms(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ULL) / 10000;
}

// Log levels from [log] level in rekindle.ini; the file is created on the first line written.
enum { LOG_OFF, LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG };
static const char *const level_names[] = {"OFF", "ERROR", "WARN", "INFO", "DEBUG"};
static int log_level = LOG_ERROR;
static char log_path[MAX_PATH];
static FILE *out;
static bool log_failed;

// Takes the lock and writes the line prefix; returns NULL with the lock released when nothing is logged.
static FILE *log_begin(int level)
{
    if (level > log_level || !log_path[0])
        return NULL;
    EnterCriticalSection(&lock);
    if (!out && !log_failed)
        log_failed = !(out = fopen(log_path, "w"));
    if (!out) {
        LeaveCriticalSection(&lock);
        return NULL;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(out, "%02d:%02d:%02d.%03d %-5s ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            level_names[level]);
    return out;
}

static void log_end(void)
{
    fflush(out);
    LeaveCriticalSection(&lock);
}

static void log_line(int level, const char *fmt, ...)
{
    if (!log_begin(level))
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    log_end();
}

// Debug only: a header line, then the payload escaped so the log stays one text file.
static void log_event(const char *what, void *h, const char *info, const void *data, size_t len)
{
    if (!log_begin(LOG_DEBUG))
        return;
    fprintf(out, "+%llu tid=%lu %s h=%p %s\n", (unsigned long long)(unix_ms() - t0), GetCurrentThreadId(), what, h,
            info ? info : "");
    if (data && len) {
        const uint8_t *p = data;
        fputs("  | ", out);
        for (size_t i = 0; i < len; i++) {
            if (p[i] == '\n')
                fputs(i + 1 < len ? "\n  | " : "", out);
            else if (p[i] == '\r')
                ;
            else if (p[i] >= 0x20 && p[i] < 0x7f && p[i] != '\\')
                fputc(p[i], out);
            else
                fprintf(out, "\\x%02x", p[i]);
        }
        fputc('\n', out);
    }
    log_end();
}

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

// ---- Local server: without cl_server_redirector every WinHttpConnect goes to this loopback server ----

static uint64_t fake_self;
static volatile LONG local_port;
static volatile LONG next_id;

// Copies the balanced JSON object or array that follows "key": in src, or "{}" when it is missing.
static void json_part(char *dst, size_t cap, const char *src, const char *key)
{
    char k[64];
    snprintf(k, sizeof k, "\"%s\":", key);
    const char *p = strstr(src, k);
    snprintf(dst, cap, "{}");
    if (!p || (p[strlen(k)] != '{' && p[strlen(k)] != '['))
        return;
    p += strlen(k);
    int depth = 0;
    bool str = false;
    for (const char *q = p; *q; q++) {
        if (str)
            str = *q == '\\' ? (q++, true) : *q != '"';
        else if (*q == '"')
            str = true;
        else if (*q == '{' || *q == '[')
            depth++;
        else if ((*q == '}' || *q == ']') && !--depth) {
            snprintf(dst, cap, "%.*s", (int)(q + 1 - p), p);
            return;
        }
    }
}

static long long json_int(const char *src, const char *key)
{
    char k[64];
    snprintf(k, sizeof k, "\"%s\":", key);
    const char *p = strstr(src, k);
    return p ? strtoll(p + strlen(k) + (p[strlen(k)] == '"'), NULL, 10) : 0;
}

// Fills out with the value object for one request; returns false for the long polls that never get an answer.
static bool local_answer(const char *path, const char *body, char *type, size_t tcap, char *out, size_t cap)
{
    long long now = (long long)(unix_ms() / 1000);
    const char *t = strstr(body, "\"type\":\"Request");
    if (strstr(path, "/redirect"))
        snprintf(type, tcap, "CreateSession");
    else if (t)
        snprintf(type, tcap, "%.*s", (int)strcspn(t + 15, "\""), t + 15);
    else
        snprintf(type, tcap, "Unknown");
    size_t n = strlen(type);
    if (n > 6 && !strcmp(type + n - 6, "Params"))
        type[n - 6] = 0;
    char part[2048];
    // Like the real server, only the first notification fetch per session is answered; the game re-asks at once.
    static volatile LONG notified;
    if (!strcmp(type, "Polling") || (!strcmp(type, "GetNotificationMessage") && InterlockedExchange(&notified, 1)))
        return false;
    if (!strcmp(type, "CreateSession")) {
        InterlockedExchange(&notified, 0);
        snprintf(out, cap,
                 "{\"player_id\":1,\"user_id\":\"%016llx\",\"client_ip\":\"127.0.0.1\",\"refresh_token\":{\"client_id\":"
                 "\"1\",\"created_at\":\"%lld\",\"expires_at\":\"%lld\",\"signature\":\"signature\"},\"redirect_addr\":\"\"}",
                 (unsigned long long)(fake_self ? fake_self : 0x0110000100000001ULL), now, now + 7200);
    } else if (!strcmp(type, "GetNotificationMessage"))
        snprintf(out, cap,
                 "{\"messages\":[],\"in_campaign\":true,\"campaign_info\":{\"campaign_id\":-1,\"start_at\":{\"seconds\":"
                 "\"%lld\"},\"end_at\":{\"seconds\":\"%lld\"},\"target_boss\":[\"BOSS_00\",\"BOSS_01\",\"BOSS_02\","
                 "\"BOSS_03\",\"BOSS_04\",\"BOSS_05\",\"BOSS_06\",\"BOSS_08\"],\"reward\":67},\"enable_chaos\":true}",
                 now, now + 2592000);
    else if (!strcmp(type, "GetAnnounceMessageList"))
        snprintf(out, cap, "{\"to_all_msg\":[],\"to_player_msg\":[]}");
    else if (!strcmp(type, "UpdateLoginPlayerCharacter")) {
        long long id = json_int(body, "login_character_id");
        snprintf(out, cap, "{\"new_character_id\":%lld}", id > 0 ? id : 1);
    } else if (!strcmp(type, "GetChaosRankInfo"))
        snprintf(out, cap, "{\"rank_rate\":[999,1999,3999,5999,10000]}");
    else if (!strcmp(type, "GetChaosRate"))
        snprintf(out, cap, "{\"chaos_rate\":4250,\"last_multi_game_id\":\"0\"}");
    else if (!strcmp(type, "GetClearDataList"))
        snprintf(out, cap, "{\"clear_data_list\":[]}");
    else if (!strcmp(type, "ServerPing"))
        snprintf(out, cap, "{\"millisecond\":\"0\"}");
    else if (!strcmp(type, "GetMultiGameID"))
        snprintf(out, cap, "{\"multi_game_id\":\"%lld\",\"report_count_list\":[]}",
                 now - 1700000000 + InterlockedIncrement(&next_id));
    else if (!strcmp(type, "GetDeathBoxDataList"))
        snprintf(out, cap, "{\"death_box_data_list\":[]}");
    else if (!strcmp(type, "GetDeathBoxUserInfoList"))
        snprintf(out, cap, "{\"death_box_user_info_list\":[]}");
    else if (!strcmp(type, "GetGhostDataList"))
        snprintf(out, cap, "{\"ghost_data_list\":[]}");
    else if (!strcmp(type, "GetNamedWeapon")) {
        const char *d = strstr(body, "\"death_box_id\":\"");
        snprintf(out, cap, "{\"death_box_id\":\"%.*s\",\"weapon_info_list\":[]}", d ? (int)strcspn(d + 16, "\"") : 1,
                 d ? d + 16 : "0");
    } else if (!strcmp(type, "CreateDeathBoxData")) {
        json_part(part, sizeof part, body, "death_data");
        snprintf(out, cap,
                 "{\"death_box_data\":{\"id\":\"%ld\",\"play_mode\":\"OTHER\",\"death_data\":%s,\"item_data\":"
                 "{\"named_weapon_id_list\":[],\"other_list\":[]},\"chr_init_param_id\":%lld,\"codenames\":[]}}",
                 InterlockedIncrement(&next_id), part, json_int(body, "chr_init_param_id"));
    } else if (!strcmp(type, "JoinMultiplay"))
        snprintf(out, cap, "{\"keep_interval\":60}");
    else if (!strcmp(type, "CheckPenaltyMultiplay"))
        snprintf(out, cap, "{\"forgiven\":false,\"is_penalty\":false,\"until_no_penalty_sec\":\"-1\"}");
    else
        snprintf(out, cap, "{}");
    return true;
}

// One keep-alive connection: reads requests until the client closes.
static DWORD WINAPI local_conn(LPVOID arg)
{
    SOCKET s = (SOCKET)(uintptr_t)arg;
    char *req = malloc(1 << 20), *value = malloc(1 << 16), *reply = malloc((1 << 16) + 512);
    int have = 0;
    for (;;) {
        char *end = NULL;
        while (!(end = have ? strstr(req, "\r\n\r\n") : NULL)) {
            int got = have < (1 << 20) - 1 ? recv(s, req + have, (1 << 20) - 1 - have, 0) : 0;
            if (got <= 0)
                goto done;
            have += got, req[have] = 0;
        }
        const char *cl = NULL;
        for (const char *q = req; q < end && !cl; q++)
            if (!_strnicmp(q, "\r\nContent-Length:", 17))
                cl = q + 2;
        int head = (int)(end + 4 - req), len = cl && cl < end ? atoi(cl + 15) : 0;
        while (have < head + len && have < (1 << 20) - 1) {
            int got = recv(s, req + have, (1 << 20) - 1 - have, 0);
            if (got <= 0)
                goto done;
            have += got, req[have] = 0;
        }
        char path[128], type[96], saved = req[head + len];
        snprintf(path, sizeof path, "%.*s", (int)strcspn(req, "\r\n"), req);
        req[head + len] = 0;
        bool answer = local_answer(path, req + head, type, sizeof type, value, 1 << 16);
        req[head + len] = saved;
        log_event("LocalServer", NULL, type, NULL, 0);
        if (!answer) {
            // Hold the long poll until the game times out and closes it.
            char sink[512];
            while (recv(s, sink, sizeof sink, 0) > 0)
                ;
            goto done;
        }
        int body = snprintf(reply, (1 << 16) + 512, "{\"type\":\"Response%sParams\",\"value\":%s}", type, value);
        char hdr[256];
        int hl = snprintf(hdr, sizeof hdr,
                          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nSet-Cookie: "
                          "session-id=local; Path=/\r\n\r\n",
                          body);
        send(s, hdr, hl, 0);
        send(s, reply, body, 0);
        memmove(req, req + head + len, have - head - len);
        have -= head + len, req[have] = 0;
    }
done:
    closesocket(s);
    free(req), free(value), free(reply);
    return 0;
}

static DWORD WINAPI local_server(LPVOID ready)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int alen = sizeof a;
    if (l == INVALID_SOCKET || bind(l, (struct sockaddr *)&a, sizeof a) || listen(l, 16) ||
        getsockname(l, (struct sockaddr *)&a, &alen)) {
        log_line(LOG_ERROR, "Local server: listen failed, error=%d", WSAGetLastError());
        SetEvent(ready);
        return 0;
    }
    InterlockedExchange(&local_port, ntohs(a.sin_port));
    log_line(LOG_INFO, "Local server on 127.0.0.1:%ld", local_port);
    SetEvent(ready);
    for (SOCKET c; (c = accept(l, NULL, NULL)) != INVALID_SOCKET;)
        CloseHandle(CreateThread(NULL, 0, local_conn, (LPVOID)(uintptr_t)c, 0, NULL));
    return 0;
}

// True when the redirector is absent, so the local server answers. Starts the server on first use.
static bool local_mode(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static bool on;
    BOOL pending;
    if (InitOnceBeginInitialize(&once, 0, &pending, NULL) && pending) {
        on = !GetModuleHandleA("cl_server_redirector.dll");
        if (on) {
            HANDLE ready = CreateEventA(NULL, TRUE, FALSE, NULL);
            CloseHandle(CreateThread(NULL, 0, local_server, ready, 0, NULL));
            WaitForSingleObject(ready, 5000);
            CloseHandle(ready);
        }
        InitOnceComplete(&once, 0, NULL);
    }
    return on;
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

// ---- Steam: ISteamMatchmaking, ISteamFriends and callbacks ----

// Original vtables per wrapped interface; the asm handlers index them by key >> 16.
void **steam_orig_tables[16];
static struct {
    const char *name;
    void **orig, **wrapped;
} ifaces[16];
static int iface_count;

static const char *mm_names[] = {
    "GetFavoriteGameCount", "GetFavoriteGame", "AddFavoriteGame", "RemoveFavoriteGame", "RequestLobbyList",
    "AddRequestLobbyListStringFilter", "AddRequestLobbyListNumericalFilter", "AddRequestLobbyListNearValueFilter",
    "AddRequestLobbyListFilterSlotsAvailable", "AddRequestLobbyListDistanceFilter",
    "AddRequestLobbyListResultCountFilter", "AddRequestLobbyListCompatibleMembersFilter", "GetLobbyByIndex",
    "CreateLobby", "JoinLobby", "LeaveLobby", "InviteUserToLobby", "GetNumLobbyMembers", "GetLobbyMemberByIndex",
    "GetLobbyData", "SetLobbyData", "GetLobbyDataCount", "GetLobbyDataByIndex", "DeleteLobbyData",
    "GetLobbyMemberData", "SetLobbyMemberData", "SendLobbyChatMsg", "GetLobbyChatEntry", "RequestLobbyData",
    "SetLobbyGameServer", "GetLobbyGameServer", "SetLobbyMemberLimit", "GetLobbyMemberLimit", "SetLobbyType",
    "SetLobbyJoinable", "GetLobbyOwner", "SetLobbyOwner", "SetLinkedLobby",
};

void steam_log_call(uint64_t key, uint64_t *args, uint64_t ret, int post);

// Post handler calls the original and logs the result; pre handler logs, then tail-jumps (for stack-arg methods).
__asm__(".globl steam_thunk_post\n"
        "steam_thunk_post:\n"
        "  push %rbx\n"
        "  sub $0x50, %rsp\n"
        "  mov %r10, %rbx\n"
        "  mov %rcx, 0x20(%rsp)\n"
        "  mov %rdx, 0x28(%rsp)\n"
        "  mov %r8, 0x30(%rsp)\n"
        "  mov %r9, 0x38(%rsp)\n"
        "  mov %rbx, %rax\n"
        "  shr $16, %rax\n"
        "  lea steam_orig_tables(%rip), %r11\n"
        "  mov (%r11,%rax,8), %r11\n"
        "  movzwl %bx, %eax\n"
        "  call *(%r11,%rax,8)\n"
        "  mov %rax, 0x40(%rsp)\n"
        "  movq %xmm0, 0x48(%rsp)\n"
        "  mov %rbx, %rcx\n"
        "  lea 0x20(%rsp), %rdx\n"
        "  mov %rax, %r8\n"
        "  mov $1, %r9d\n"
        "  call steam_log_call\n"
        "  mov 0x40(%rsp), %rax\n"
        "  movq 0x48(%rsp), %xmm0\n"
        "  add $0x50, %rsp\n"
        "  pop %rbx\n"
        "  ret\n"
        ".globl steam_thunk_pre\n"
        "steam_thunk_pre:\n"
        "  sub $0x68, %rsp\n"
        "  mov %rcx, 0x20(%rsp)\n"
        "  mov %rdx, 0x28(%rsp)\n"
        "  mov %r8, 0x30(%rsp)\n"
        "  mov %r9, 0x38(%rsp)\n"
        "  movq %xmm0, 0x40(%rsp)\n"
        "  movq %xmm1, 0x48(%rsp)\n"
        "  movq %xmm2, 0x50(%rsp)\n"
        "  movq %xmm3, 0x58(%rsp)\n"
        "  mov %r10, 0x60(%rsp)\n"
        "  mov %r10, %rcx\n"
        "  lea 0x20(%rsp), %rdx\n"
        "  xor %r8d, %r8d\n"
        "  xor %r9d, %r9d\n"
        "  call steam_log_call\n"
        "  mov 0x20(%rsp), %rcx\n"
        "  mov 0x28(%rsp), %rdx\n"
        "  mov 0x30(%rsp), %r8\n"
        "  mov 0x38(%rsp), %r9\n"
        "  movq 0x40(%rsp), %xmm0\n"
        "  movq 0x48(%rsp), %xmm1\n"
        "  movq 0x50(%rsp), %xmm2\n"
        "  movq 0x58(%rsp), %xmm3\n"
        "  mov 0x60(%rsp), %r10\n"
        "  add $0x68, %rsp\n"
        "  mov %r10, %rax\n"
        "  shr $16, %rax\n"
        "  lea steam_orig_tables(%rip), %r11\n"
        "  mov (%r11,%rax,8), %r11\n"
        "  movzwl %r10w, %eax\n"
        "  jmp *(%r11,%rax,8)\n");
void steam_thunk_post(void);
void steam_thunk_pre(void);

static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION m;
    if ((uintptr_t)p < 0x10000 || !VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT ||
        (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(m.Protect & 0xee))
        return 0;
    return (uint8_t *)p + n <= (uint8_t *)m.BaseAddress + m.RegionSize;
}

// Prints a pointer as a quoted string when it points at printable text, else as hex.
static int fmt_arg(char *dst, size_t cap, uint64_t v)
{
    const char *s = (const char *)(uintptr_t)v;
    MEMORY_BASIC_INFORMATION m;
    if (readable(s, 1) && VirtualQuery(s, &m, sizeof m)) {
        size_t room = (size_t)((const char *)m.BaseAddress + m.RegionSize - s), n = 0;
        while (n < 120 && n < room && s[n] >= 0x20 && s[n] < 0x7f)
            n++;
        if (n && (n >= 120 || (n < room && s[n] == 0)))
            return snprintf(dst, cap, "\"%.*s\"", (int)n, s);
    }
    return snprintf(dst, cap, "0x%llx", (unsigned long long)v);
}

// Debug only: logs a line the first time and again at power-of-two repeat counts, so polling stays small.
static void log_dedup(const char *what, const char *line)
{
    if (log_level < LOG_DEBUG)
        return;
    static uint64_t keys[8192];
    static uint32_t counts[8192];
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = what; *p; p++)
        h = (h ^ (uint8_t)*p) * 1099511628211ULL;
    for (const char *p = line; *p; p++)
        h = (h ^ (uint8_t)*p) * 1099511628211ULL;
    EnterCriticalSection(&lock);
    uint32_t i = h & 8191, c = 0;
    while (keys[i] && keys[i] != h)
        i = (i + 1) & 8191;
    keys[i] = h;
    c = ++counts[i];
    LeaveCriticalSection(&lock);
    if (c & (c - 1))
        return;
    char buf[1500];
    snprintf(buf, sizeof buf, c > 1 ? "%s (x%u)" : "%s", line, c);
    log_line(LOG_DEBUG, "%s %s", what, buf);
}

static void wrap_iface(void *obj, const char *ver);
static void note(int level, const char *fmt, ...);

void steam_log_call(uint64_t key, uint64_t *args, uint64_t ret, int post)
{
    int k = (int)(key >> 16), slot = (int)(key & 0xffff);
    const char *name = ifaces[k].name;
    // The client only matters for the interfaces it hands out.
    if (!strncmp(name, "SteamClient", 11)) {
        // GetISteamUtils takes (pipe, version); the other getters take (user, pipe, version).
        uint64_t ver = slot == 9 ? args[2] : args[3];
        if (post && (slot == 5 || slot == 8 || slot == 9 || slot == 10 || slot == 12 || slot == 16) &&
            readable((void *)(uintptr_t)ver, 1))
            wrap_iface((void *)(uintptr_t)ret, (const char *)(uintptr_t)ver);
        return;
    }
    if (log_level < LOG_DEBUG)
        return;
    int mm = !strncmp(name, "SteamMatchMaking", 16);
    char line[1200], a[3][300];
    for (int i = 0; i < 3; i++)
        fmt_arg(a[i], sizeof a[i], args[i + 1]);
    int n = snprintf(line, sizeof line, "%s.%s(%s, %s, %s)", name,
                     mm && slot < 38 ? mm_names[slot] : "", a[0], a[1], a[2]);
    if (!(mm && slot < 38))
        n = snprintf(line, sizeof line, "%s.slot%d(%s, %s, %s)", name, slot, a[0], a[1], a[2]);
    if (post) {
        char r[300];
        // CSteamID returns go through a hidden pointer, so rax points at the id.
        if (mm && (slot == 12 || slot == 18 || slot == 35) && readable((void *)(uintptr_t)ret, 8))
            snprintf(r, sizeof r, "id=%llu", *(unsigned long long *)(uintptr_t)ret);
        else if (mm && (slot == 19 || slot == 24))
            fmt_arg(r, sizeof r, ret);
        else
            snprintf(r, sizeof r, "0x%llx", (unsigned long long)ret);
        snprintf(line + n, sizeof line - n, " -> %s", r);
    }
    log_dedup("Steam", line);
}

// Fake lobby mode: Steam's answers for fake_lobby are rewritten so a rejoin to a dead lobby looks like a live
// one owned by fake_host.
static uint64_t fake_lobby, fake_host, fake_self, fake_join_call;
#define DEFAULT_LOBBY 0x186000048928a92ULL
#define DEFAULT_HOST 76561200107749360ULL
typedef void (*Run_t)(void *, void *, uint64_t, uint64_t);
static void **mm_orig, **utils_orig;

typedef uint64_t *(*GetId_t)(void *, uint64_t *, uint64_t);
typedef uint64_t *(*GetMember_t)(void *, uint64_t *, uint64_t, int);
typedef int (*GetNum_t)(void *, uint64_t);
typedef uint64_t (*Join_t)(void *, uint64_t);
typedef bool (*Done_t)(void *, uint64_t, bool *);
typedef bool (*Result_t)(void *, uint64_t, void *, int, int, bool *);

// Restore events; debug repeats are thinned out like the Steam call log.
static void note(int level, const char *fmt, ...)
{
    if (level > log_level)
        return;
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (level == LOG_DEBUG)
        log_dedup("Fake", line);
    else
        log_line(level, "%s", line);
}

// Lobby relay for co-op rejoin after everyone crashed. The first player to restore creates a public lobby
// tagged nrk_mgid=<mgid>; another player restoring the same run finds it by lobby search and maps its own id to it.
static uint64_t game_lobby, real_lobby, create_call, search_call, found_lobby, restore_mgid;
static bool lobby_decided;
static void *mm_self, *utils_self;
static void *mm_stub[44];
typedef uint64_t (*Fwd_t)(void *, uint64_t, uint64_t, uint64_t);
typedef uint64_t (*Create_t)(void *, int, int);
typedef void (*StrFilter_t)(void *, const char *, const char *, int);
typedef void (*IntFilter_t)(void *, int);
typedef uint64_t (*Call_t)(void *);
#define RELAY_KEY "nrk_mgid"

static uint64_t tr(uint64_t v)
{
    return real_lobby && v == game_lobby ? real_lobby : v;
}

// The game's first RequestLobbyData during a restore decides: join a found relay lobby, or create one.
static void relay_decide(uint64_t lobby)
{
    if (lobby_decided || !restore_mgid || !mm_self)
        return;
    lobby_decided = true;
    if (found_lobby) {
        game_lobby = lobby, real_lobby = found_lobby;
        fake_lobby = 0, fake_host = 0;
        note(LOG_INFO, "Relay join 0x%llx as 0x%llx", (unsigned long long)found_lobby, (unsigned long long)lobby);
    } else if (lobby == fake_lobby) {
        game_lobby = lobby;
        create_call = ((Create_t)mm_orig[13])(mm_self, 2, 3);
        note(LOG_INFO, "Relay create call=0x%llx", (unsigned long long)create_call);
    }
}

static bool call_done(uint64_t call, void *buf, int cub, int id)
{
    bool failed = false;
    if (!((Done_t)utils_orig[11])(utils_self, call, &failed))
        return false;
    memset(buf, 0, cub);
    if (!((Result_t)utils_orig[13])(utils_self, call, buf, cub, id, &failed) || failed)
        memset(buf, 0xff, cub);
    return true;
}

// Runs from RunCallbacks: searches for a relay lobby until the restore decides, and finishes CreateLobby.
static void relay_tick(void)
{
    static uint64_t search_at;
    uint64_t now = unix_ms();
    if (!mm_self || !utils_self)
        return;
    if (restore_mgid && !lobby_decided && !search_call && now - search_at > 5000) {
        char v[24];
        snprintf(v, sizeof v, "%llu", (unsigned long long)restore_mgid);
        search_at = now;
        ((StrFilter_t)mm_orig[5])(mm_self, RELAY_KEY, v, 0);
        ((IntFilter_t)mm_orig[9])(mm_self, 3);
        search_call = ((Call_t)mm_orig[4])(mm_self);
    }
    if (search_call) {
        // LobbyMatchList_t (510): lobbies matching u32.
        uint32_t r;
        if (call_done(search_call, &r, 4, 510)) {
            search_call = 0;
            if (r != 0xffffffff && r > 0 && !lobby_decided) {
                ((GetId_t)mm_orig[12])(mm_self, &found_lobby, 0);
                note(LOG_INFO, "Relay found %u, lobby 0x%llx", r, (unsigned long long)found_lobby);
            }
        }
    }
    if (create_call) {
        // LobbyCreated_t (513): result u32, lobby u64 at 8.
        uint8_t r[16];
        if (!call_done(create_call, r, 16, 513))
            return;
        create_call = 0;
        uint32_t res;
        memcpy(&res, r, 4);
        if (res != 1) {
            note(LOG_WARN, "Relay create failed result=%u", res);
            return;
        }
        char v[24];
        snprintf(v, sizeof v, "%llu", (unsigned long long)restore_mgid);
        memcpy(&real_lobby, r + 8, 8);
        ((Fwd_t)mm_stub[20])(mm_self, real_lobby, (uint64_t)"inviteParam", (uint64_t)"0");
        ((Fwd_t)mm_stub[20])(mm_self, real_lobby, (uint64_t)RELAY_KEY, (uint64_t)v);
        note(LOG_INFO, "Relay lobby 0x%llx", (unsigned long long)real_lobby);
    }
}

// Matchmaking slots without stack arguments: map the game's lobby id to the real one, then log and call.
#define TR_SLOT(n) \
    static uint64_t tr##n(void *s, uint64_t a, uint64_t b, uint64_t c) \
    { \
        return ((Fwd_t)mm_stub[n])(s, tr(a), tr(b), tr(c)); \
    }
TR_SLOT(16) TR_SLOT(19) TR_SLOT(20) TR_SLOT(21) TR_SLOT(23) TR_SLOT(24) TR_SLOT(25) TR_SLOT(26)
TR_SLOT(31) TR_SLOT(32) TR_SLOT(33) TR_SLOT(34) TR_SLOT(36) TR_SLOT(37)

static void reset_mates(void);
static uint64_t tr13(void *s, uint64_t a, uint64_t b, uint64_t c)
{
    reset_mates();
    return ((Fwd_t)mm_stub[13])(s, a, b, c);
}

static uint64_t tr15(void *s, uint64_t a, uint64_t b, uint64_t c)
{
    if (real_lobby && tr(a) == real_lobby)
        note(LOG_INFO, "Relay leave 0x%llx", (unsigned long long)real_lobby);
    return ((Fwd_t)mm_stub[15])(s, tr(a), b, c);
}

static uint64_t tr28(void *s, uint64_t a, uint64_t b, uint64_t c)
{
    mm_self = s;
    relay_decide(a);
    return ((Fwd_t)mm_stub[28])(s, tr(a), b, c);
}

// After the replay the fake host leaves the lobby like a crashed host: 506 at host_gone_at, then self owns it.
static uint64_t host_gone_at;
static volatile LONG host_gone;

static uint64_t *fk_owner(void *self, uint64_t *ret, uint64_t lobby)
{
    if (lobby != fake_lobby || (host_gone && real_lobby))
        return ((GetId_t)mm_orig[35])(self, ret, tr(lobby));
    *ret = host_gone ? fake_self : fake_host;
    note(LOG_DEBUG, "GetLobbyOwner -> %llu", (unsigned long long)*ret);
    return ret;
}

static int fk_num(void *self, uint64_t lobby)
{
    if (lobby != fake_lobby || (host_gone && real_lobby))
        return ((GetNum_t)mm_orig[17])(self, tr(lobby));
    note(LOG_DEBUG, "GetNumLobbyMembers -> %d", host_gone ? 1 : 2);
    return host_gone ? 1 : 2;
}

static uint64_t *fk_member(void *self, uint64_t *ret, uint64_t lobby, int i)
{
    if (lobby != fake_lobby || (host_gone && real_lobby)) {
        ((GetMember_t)mm_orig[18])(self, ret, tr(lobby), i);
        note_mate(*ret);
        return ret;
    }
    *ret = i == 0 ? fake_self : i == 1 && !host_gone ? fake_host : 0;
    note(LOG_DEBUG, "GetLobbyMemberByIndex %d -> %llu", i, (unsigned long long)*ret);
    return ret;
}

// FUN_140b5da90 sets the "host lost" leave reason ([0x143c046a8]+0x13 = 1) that sends the game offline.
// During a restored run the fake host always leaves, so write 0 instead.
static bool keep_online_ok;
// FUN_140df6890 sets +0x14 when self becomes lobby owner but the session can't take over; skip it too.
// Writes new over old at address at, only while the original bytes are still there.
static bool patch_bytes(uintptr_t at, const uint8_t *old_b, const uint8_t *new_b, size_t n)
{
    uint8_t *p = (uint8_t *)at;
    DWORD old;
    if (memcmp(p, old_b, n) || !VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(p, new_b, n);
    VirtualProtect(p, n, old, &old);
    return true;
}

// The AFK kick is a local idle timer in FUN_1403ba240 (warning at 270 s, kick at 300 s, from
// GameSystemCommonParam). Moving always counts as active, and the kick flag write stores 0.
static DWORD WINAPI startup_patches(LPVOID unused)
{
    (void)unused;
    static const uint8_t move_old[] = {0x0f,0x2f,0x05,0xa2,0x02,0x5d,0x03,0x72,0x0e,0x33,0xff};
    static const uint8_t move_new[] = {0x0f,0x2f,0x05,0xa2,0x02,0x5d,0x03,0x90,0x90,0x33,0xff};
    static const uint8_t kick_old[] = {0x0f,0x2f,0xc8,0x76,0x19,0x66,0xc7,0x83,0xc4,0x01,0x00,0x00,0x01,0x01};
    static const uint8_t kick_new[] = {0x0f,0x2f,0xc8,0x76,0x19,0x66,0xc7,0x83,0xc4,0x01,0x00,0x00,0x00,0x00};
    // Rejoin penalty: CS::RejoinMapEnterStep calls FUN_140729200(mgr, skip=0), which takes a level, drops the
    // runes in a death box and sends the die log. skip=1 is what a penalty-free respawn passes.
    static const uint8_t pen1_old[] = {0x33,0xd2,0xe8,0x21,0x9f,0xae,0xff,0x83,0xbb,0xac,0x00,0x00,0x00,0xff};
    static const uint8_t pen1_new[] = {0xb2,0x01,0xe8,0x21,0x9f,0xae,0xff,0x83,0xbb,0xac,0x00,0x00,0x00,0xff};
    static const uint8_t pen2_old[] = {0x33,0xd2,0xe8,0x1b,0x9d,0xae,0xff,0x48,0x8b,0x05,0xbc,0xfb,0xfc,0x02};
    static const uint8_t pen2_new[] = {0xb2,0x01,0xe8,0x1b,0x9d,0xae,0xff,0x48,0x8b,0x05,0xbc,0xfb,0xfc,0x02};
    // A player who crashed while downed would come back downed; always take the respawn branch instead.
    static const uint8_t down_old[] = {0x84,0xc0,0x74,0x07,0x80,0x8b,0xc2,0x00,0x00,0x00,0x01,0x48,0x8b,0x3d};
    static const uint8_t down_new[] = {0x84,0xc0,0xeb,0x07,0x80,0x8b,0xc2,0x00,0x00,0x00,0x01,0x48,0x8b,0x3d};
    bool afk = cfg("penalties", "remove_afk_kick", 1), pen = cfg("penalties", "remove_rejoin_penalty", 1);
    bool m = !afk, k = !afk, p1 = !pen, p2 = !pen, d = !cfg("penalties", "respawn_if_downed", 1);
    for (int i = 0; i < 1200 && !(m && k && p1 && p2 && d); i++, Sleep(250)) {
        m = m || patch_bytes(0x1403ba2cf, move_old, move_new, sizeof move_old);
        k = k || patch_bytes(0x1403ba34a, kick_old, kick_new, sizeof kick_old);
        p1 = p1 || patch_bytes(0x140c3f2d8, pen1_old, pen1_new, sizeof pen1_old);
        p2 = p2 || patch_bytes(0x140c3f4de, pen2_old, pen2_new, sizeof pen2_old);
        d = d || patch_bytes(0x140c3fabc, down_old, down_new, sizeof down_old);
    }
    note(LOG_INFO, "Startup patches afk=%d/%d penalty=%d/%d downed=%d settings=%d%d%d", m, k, p1, p2, d, afk, pen,
         cfg("penalties", "respawn_if_downed", 1));
    if (!(m && k && p1 && p2 && d))
        note(LOG_WARN, "Startup patches not applied in time, the game bytes did not match");
    return 0;
}

static bool patch_reason(uintptr_t at, const uint8_t code[10], bool on)
{
    uint8_t *p = (uint8_t *)at;
    DWORD old;
    if (memcmp(p, code, 10) || !VirtualProtect(p + 10, 1, PAGE_EXECUTE_READWRITE, &old))
        return false;
    p[10] = on ? 0 : 1;
    VirtualProtect(p + 10, 1, old, &old);
    return true;
}

static void keep_online(bool on)
{
    static const uint8_t lost[] = {0x48, 0x8b, 0x05, 0x34, 0x68, 0x0a, 0x03, 0xc6, 0x40, 0x13};
    static const uint8_t owner[] = {0x48, 0x8b, 0x05, 0xc5, 0xdc, 0xe0, 0x02, 0xc6, 0x40, 0x14};
    if (!patch_reason(0x140b5de6d, lost, on)) {
        note(LOG_ERROR, "Keep online: patch bytes mismatch at 0x140b5de6d");
        return;
    }
    bool o = patch_reason(0x140df69dc, owner, on);
    if (!o)
        note(LOG_ERROR, "Keep online: patch bytes mismatch at 0x140df69dc");
    keep_online_ok = on;
    note(LOG_INFO, "Keep online %d owner=%d", on, o);
}

static uint64_t fk_join(void *self, uint64_t lobby)
{
    reset_mates();
    uint64_t call = ((Join_t)mm_orig[14])(self, lobby == fake_lobby ? lobby : tr(lobby));
    if (lobby == fake_lobby) {
        fake_join_call = call;
        run_restored = true;
        if (cfg("restore", "keep_online", 1))
            keep_online(true);
        note(LOG_INFO, "JoinLobby call=0x%llx", (unsigned long long)call);
    }
    return call;
}

static bool fk_done(void *self, uint64_t call, bool *failed)
{
    bool done = ((Done_t)utils_orig[11])(self, call, failed);
    if (call && call == fake_join_call) {
        note(LOG_DEBUG, "IsAPICallCompleted join real=%d failed=%d", done, failed ? *failed : -1);
        if (failed)
            *failed = false;
        return true;
    }
    return done;
}

// LobbyEnter_t (504): lobby u64, permissions u32 at 8, locked at 12, response u32 at 16 (1 = success).
static bool fk_result(void *self, uint64_t call, void *buf, int cub, int expected, bool *failed)
{
    bool ok = ((Result_t)utils_orig[13])(self, call, buf, cub, expected, failed);
    for (int o = 0; ok && real_lobby && o + 8 <= cub; o += 8)
        if (*(uint64_t *)((uint8_t *)buf + o) == real_lobby)
            *(uint64_t *)((uint8_t *)buf + o) = game_lobby;
    if (!call || call != fake_join_call || expected != 504 || cub < 20)
        return ok;
    note(LOG_DEBUG, "GetAPICallResult join real=%d response=%u", ok, ok ? *(uint32_t *)((uint8_t *)buf + 16) : 0);
    memset(buf, 0, cub);
    *(uint64_t *)buf = fake_lobby;
    *(uint32_t *)((uint8_t *)buf + 16) = 1;
    if (failed)
        *failed = false;
    return true;
}

// ---- Fake host: answers the game's P2P link to fake_host on ISteamNetworking006 channel 30 ----
// Raw handshake, AES-CTR frames, reliable hello and acks, then plays the answers built from the run's snapshot.
#define EMU_CHANNEL 30
#define EMU_SEQ0 0xc594
static CRITICAL_SECTION emu_lock;
static BCRYPT_KEY_HANDLE emu_key;
static uint32_t emu_game_nonce, emu_host_nonce, emu_crc[256];
static uint16_t emu_wseq;
static int emu_state;
static uint64_t emu_t0;
static uint8_t *emu_replay, *emu_answers, *emu_stream;
static size_t emu_replay_len, emu_replay_pos, emu_answers_len, emu_stream_len, emu_stream_pos;
static uint64_t emu_stream_t0;
static struct {
    uint16_t len;
    uint8_t data[1300];
} emu_q[4096];
static unsigned emu_qhead, emu_qtail;
static void **net_orig;

static void put_be16(uint8_t *p, uint16_t v) { p[0] = v >> 8, p[1] = (uint8_t)v; }
static void put_be32(uint8_t *p, uint32_t v) { put_be16(p, v >> 16), put_be16(p + 2, (uint16_t)v); }
static uint16_t get_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static uint32_t emu_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffff;
    while (n--)
        c = emu_crc[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static void emu_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xedb88320 ^ (c >> 1) : c >> 1;
        emu_crc[i] = c;
    }
    BCRYPT_ALG_HANDLE alg;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0);
    BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
    BCryptGenerateSymmetricKey(alg, &emu_key, NULL, 0, (PUCHAR) "he2WGvQXWXzeQxL2", 16, 0);
    emu_host_nonce = (GetTickCount() * 2654435761u) | 0x01000000;
}

// Counter block: BE32(larger nonce) BE32(smaller nonce) BE64(wire seq << 16), incremented big-endian per block.
static void emu_ctr(uint8_t *buf, size_t len, uint16_t wseq)
{
    uint8_t ctr[16] = {0}, ks[16];
    uint32_t a = emu_game_nonce, b = emu_host_nonce;
    put_be32(ctr, a > b ? a : b);
    put_be32(ctr + 4, a > b ? b : a);
    put_be16(ctr + 12, wseq);
    for (size_t i = 0; i < len; i += 16) {
        ULONG n;
        BCryptEncrypt(emu_key, ctr, 16, NULL, NULL, 0, ks, 16, &n, 0);
        for (size_t j = 0; j < 16 && i + j < len; j++)
            buf[i + j] ^= ks[j];
        for (int k = 15; k >= 0 && ++ctr[k] == 0; k--)
            ;
    }
}

static void emu_push(const uint8_t *p, size_t n)
{
    if (emu_qtail - emu_qhead >= 4096 || n > sizeof emu_q[0].data)
        return;
    emu_q[emu_qtail % 4096].len = (uint16_t)n;
    memcpy(emu_q[emu_qtail % 4096].data, p, n);
    emu_qtail++;
}

static void emu_push_enc(const uint8_t *body, size_t blen)
{
    uint8_t pkt[1300];
    size_t len = 8 + blen;
    if (len > sizeof pkt)
        return;
    pkt[0] = (uint8_t)len, pkt[1] = 0x80 | (uint8_t)(len >> 8);
    put_be16(pkt + 2, emu_wseq);
    uint32_t crc = emu_crc32(body, blen);
    memcpy(pkt + 4, &crc, 4);
    memcpy(pkt + 8, body, blen);
    emu_ctr(pkt + 4, 4 + blen, emu_wseq++);
    emu_push(pkt, len);
}

static void emu_push_hello(uint16_t ack)
{
    uint8_t b[21] = {0x01, 0, 0, 0, 0, ack ? 0x31 : 0x01, 0xff, 0x13, 0x11, 0x00, 0x03, 0xff, 0xff};
    put_be16(b + 1, EMU_SEQ0);
    put_be16(b + 3, ack);
    for (int i = 0; i < 8; i++)
        b[13 + i] = (uint8_t)(fake_host >> (56 - 8 * i));
    emu_push_enc(b, sizeof b);
}

// ---- Auto restore: run marker, save planting, and fake-host answers built from the run's snapshot ----
// run_active.txt holds the multi_game_id of the run in progress. GetMultiGameID sets it, GetResultData and
// DeleteSession clear it. Without keep_online a restored run's fake host drop also sends DeleteSession.
static char dll_dir[MAX_PATH];
static int dll_dir_len;

// Settings from rekindle.ini next to the DLL; a missing file or key keeps the default.
static int cfg(const char *section, const char *key, int def)
{
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%.*srekindle.ini", dll_dir_len, dll_dir);
    return (int)GetPrivateProfileIntA(section, key, def, path);
}
static bool emu_built;

static const uint8_t *find_bytes(const uint8_t *s, size_t n, const char *needle)
{
    size_t k = strlen(needle);
    for (size_t i = 0; k <= n && i <= n - k; i++)
        if (!memcmp(s + i, needle, k))
            return s + i;
    return NULL;
}

// Members of the lobby the game last created or joined; they join before the run has an id, so the list is
// kept from then on. run_mates.txt holds "<mgid> <id>..."; a restore uses the first id as the fake host, so the
// slot the game keeps for it matches the teammate when they rejoin.
static uint64_t run_mgid, run_mates[3];

static uint64_t emu_mate2;
// The trio teammate who is not the fake host, or 0 in a duo.
static uint64_t trio_mate(void)
{
    for (int i = 0; i < 3; i++)
        if (run_mates[i] && run_mates[i] != fake_host && run_mates[i] != fake_self)
            return run_mates[i];
    return 0;
}

static void save_mates(void)
{
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%.*srun_mates.txt", dll_dir_len, dll_dir);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "%llu", (unsigned long long)run_mgid);
    for (int i = 0; i < 3 && run_mates[i]; i++)
        fprintf(f, " %llu", (unsigned long long)run_mates[i]);
    fprintf(f, "\n");
    fclose(f);
}

static void reset_mates(void)
{
    if (!run_mgid)
        memset(run_mates, 0, sizeof run_mates);
}

static void note_mate(uint64_t id)
{
    if (id >> 52 != 0x011 || id == fake_self)
        return;
    int i = 0;
    while (i < 3 && run_mates[i] && run_mates[i] != id)
        i++;
    if (i == 3 || run_mates[i])
        return;
    run_mates[i] = id;
    if (run_mgid)
        save_mates();
    note(LOG_DEBUG, "Lobby mate %llu", (unsigned long long)id);
}

static void set_marker(const char *path, unsigned long long m)
{
    if (m == run_mgid)
        return;
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%llu\n", m);
        fclose(f);
        run_restored = false;
        keep_online(false);
        run_mgid = m;
        save_mates();
        note(LOG_INFO, "Run marker set mgid=%llu", m);
    }
}

static void run_marker_http(const void *data, size_t n)
{
    const uint8_t *s = data, *p;
    char path[MAX_PATH];
    if (!s || !n)
        return;
    snprintf(path, MAX_PATH, "%.*srun_active.txt", dll_dir_len, dll_dir);
    // The host gets the id from GetMultiGameID; a guest first sees it in its JoinMultiplay request.
    if ((find_bytes(s, n, "\"ResponseGetMultiGameIDParams\"") ||
         (find_bytes(s, n, "\"RequestJoinMultiplayParams\"") && find_bytes(s, n, "\"relation_type\":\"GUEST\""))) &&
        (p = find_bytes(s, n, "\"multi_game_id\":\""))) {
        unsigned long long m = strtoull((const char *)p + 17, NULL, 10);
        if (m)
            set_marker(path, m);
    } else if (find_bytes(s, n, "\"RequestGetResultDataParams\"") ||
               (find_bytes(s, n, "\"RequestDeleteSessionParams\"") && (!run_restored || keep_online_ok))) {
        if (DeleteFileA(path))
            note(LOG_INFO, "Run marker cleared");
        run_mgid = 0;
        memset(run_mates, 0, sizeof run_mates);
        snprintf(path, MAX_PATH, "%.*srun_mates.txt", dll_dir_len, dll_dir);
        DeleteFileA(path);
    }
}

typedef struct {
    uint8_t *p;
    size_t n, cap;
} Buf;

static void buf_add(Buf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 4096;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

// One record per frame: an answer (u8 class, u32 key) or a timed frame (u32 ms), then u16 len and the frame.
// Messages over 1137 bytes go out as 1127-byte fragments with offset and total length.
static void add_frames(Buf *b, bool timed, uint32_t t, uint8_t cls, uint32_t key, const uint8_t *m, size_t n)
{
    for (size_t off = 0; off == 0 || off < n; off += 1127) {
        uint8_t f[1160] = {1, 0, 0, 0, 0, n <= 1137 ? 3 : 7, 0xff};
        size_t fl = 7, c = n;
        if (n > 1137) {
            c = n - off < 1127 ? n - off : 1127;
            put_be16(f + 7, 0), put_be32(f + 9, (uint32_t)off), put_be32(f + 13, (uint32_t)n);
            fl = 17;
        }
        memcpy(f + fl, m + off, c);
        fl += c;
        uint16_t l = (uint16_t)fl;
        if (timed)
            buf_add(b, &t, 4);
        else
            buf_add(b, &cls, 1), buf_add(b, &key, 4);
        buf_add(b, &l, 2);
        buf_add(b, f, fl);
        if (n <= 1137)
            break;
    }
}

static void replace_all(uint8_t *m, size_t n, const void *from, const void *to, size_t k)
{
    for (size_t i = 0; k <= n && i <= n - k; i++)
        if (!memcmp(m + i, from, k))
            memcpy(m + i, to, k), i += k - 1;
}

// The snapshot is the user's own host side, so its account id becomes the fake host's.
static void sub_self(uint8_t *m, size_t n)
{
    uint8_t a[8], b[8], wa[48], wb[48];
    char da[24], db[24];
    memcpy(a, &fake_self, 8), memcpy(b, &fake_host, 8);
    replace_all(m, n, a, b, 8);
    for (int i = 0; i < 8; i++)
        a[i] = (uint8_t)(fake_self >> (56 - 8 * i)), b[i] = (uint8_t)(fake_host >> (56 - 8 * i));
    replace_all(m, n, a, b, 8);
    int la = snprintf(da, sizeof da, "%llu", (unsigned long long)fake_self);
    int lb = snprintf(db, sizeof db, "%llu", (unsigned long long)fake_host);
    if (la != lb)
        return;
    replace_all(m, n, da, db, la);
    for (int i = 0; i < la; i++)
        wa[2 * i] = da[i], wa[2 * i + 1] = 0, wb[2 * i] = db[i], wb[2 * i + 1] = 0;
    replace_all(m, n, wa, wb, 2 * la);
}

// Snapshot records: u32 ms, u8 type, u32 len, data. Type 0 with len 5 marks what follows (class, key);
// other type-0 records start with the day clock. 0x37/0x5e are the sync stream.
static bool build_answers(const uint8_t *s, size_t n, uint64_t *clock, uint8_t box[20])
{
    Buf ans = {0}, str = {0}, intro = {0};
    bool marked = false, have_box = false, have_pos = false;
    uint8_t cls = 0;
    uint32_t key = 0, s0 = 0;
    bool s0_set = false;
    *clock = 0;
    for (size_t o = 0; o + 9 <= n;) {
        uint32_t ms, l;
        memcpy(&ms, s + o, 4), memcpy(&l, s + o + 5, 4);
        uint8_t ty = s[o + 4];
        const uint8_t *d = s + o + 9;
        if (o + 9 + l > n)
            break;
        o += 9 + l;
        if (ty == 0) {
            if (l == 5)
                marked = true, cls = d[0], memcpy(&key, d + 1, 4);
            else if (l >= 8 && !*clock)
                memcpy(clock, d, 8);
            continue;
        }
        // The snapshot DLL's own position record (0xf2) is newer than any death box, so it wins.
        if (ty == 0xf2 && l == 20 && cfg("restore", "restore_position", 1)) {
            memcpy(box, d, 20), have_box = have_pos = true;
            continue;
        }
        uint8_t *m = malloc(l + 1);
        m[0] = ty, memcpy(m + 1, d, l), sub_self(m, l + 1);
        if (ty == 0x5c && l >= 0x24 && !have_pos && !have_box)
            memcpy(box, d + 0x10, 20), have_box = true;
        if (ty == 0x37 || ty == 0x5e) {
            if (!s0_set)
                s0 = ms, s0_set = true;
            add_frames(&str, true, ms - s0, 0, 0, m, l + 1);
        } else if (marked && cls == 0) {
            add_frames(&ans, false, 0, 0x42, key, m, l + 1);
        } else if (marked && cls >= 0x10 && cls < 0x20 && cls != 0x10 + 9) {
            // Builder kind to request kind: the guest asks kind 9 for builder 10 and kind 10 for builder 11.
            uint32_t bk = cls - 0x10, rk = bk == 10 ? 9 : bk == 11 ? 10 : bk;
            add_frames(&ans, false, 0, 0x51, rk, m, l + 1);
        }
        free(m);
    }
    // Player list (fake host, then the guest; ids at +5 and +0x25) and three flags, as a real host sends them.
    static const uint8_t p61[] = {
        0x61, 2, 0, 0, 0, 0xf0, 0xff, 0xff, 0x7f, 1, 0, 0x10, 1, 1, 0, 0, 0, 0xe, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
        1, 1, 0, 0, 0, 0, 0, 0, 0xbb, 0xc2, 0xf7, 0xa, 1, 0, 0x10, 1, 2, 0, 0, 0, 0xd, 0, 0, 0, 2, 0, 0, 0, 0, 0,
        0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    // In a trio the second mate gets a third entry, so the restored host keeps a slot for them as well.
    uint8_t m61[sizeof p61 + 0x20], m07[] = {0x07, 2, 0, 0, 0};
    uint64_t mate2 = emu_mate2 = trio_mate();
    memcpy(m61, p61, sizeof p61), memcpy(m61 + 5, &fake_host, 8), memcpy(m61 + 0x25, &fake_self, 8);
    if (mate2) {
        memcpy(m61 + 0x45, p61 + 0x25, 0x20), memcpy(m61 + 0x45, &mate2, 8);
        m61[1] = m07[1] = 3, m61[0x45 + 8] = m61[0x45 + 0x10] = 3;
    }
    static const uint8_t m27[] = {0x27, 1, 0, 0, 0}, m49[] = {0x49, 0, 0, 0, 0};
    add_frames(&intro, true, 101, 0, 0, m61, mate2 ? sizeof m61 : sizeof p61);
    add_frames(&intro, true, 101, 0, 0, m07, 5);
    add_frames(&intro, true, 101, 0, 0, m27, 5);
    add_frames(&intro, true, 169, 0, 0, m49, 5);
    EnterCriticalSection(&emu_lock);
    emu_replay = intro.p, emu_replay_len = intro.n;
    emu_answers = ans.p, emu_answers_len = ans.n;
    emu_stream = str.p, emu_stream_len = str.n;
    emu_built = true;
    LeaveCriticalSection(&emu_lock);
    note(LOG_DEBUG, "Auto answers=%u stream=%u intro=%u", (unsigned)ans.n, (unsigned)str.n, (unsigned)intro.n);
    return have_box;
}

static bool aes_cbc(bool enc, uint8_t *data, ULONG n, const uint8_t iv_in[16])
{
    static const uint8_t k[16] = {0x18, 0xf6, 0x32, 0x66, 0x05, 0xbd, 0x17, 0x8a,
                                  0x55, 0x24, 0x52, 0x3a, 0xc0, 0xa0, 0xc6, 0x09};
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_KEY_HANDLE key;
    uint8_t iv[16];
    ULONG got = 0;
    memcpy(iv, iv_in, 16);
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0))
        return false;
    BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    NTSTATUS st = BCryptGenerateSymmetricKey(alg, &key, NULL, 0, (PUCHAR)k, 16, 0);
    if (!st)
        st = enc ? BCryptEncrypt(key, data, n, NULL, iv, 16, data, n, &got, 0)
                 : BCryptDecrypt(key, data, n, NULL, iv, 16, data, n, &got, 0);
    if (!st)
        BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(alg, 0);
    return !st && got == n;
}

static void md5(const uint8_t *d, ULONG n, uint8_t out[16])
{
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE h;
    memset(out, 0, 16);
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, NULL, 0))
        return;
    if (!BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0)) {
        BCryptHashData(h, (PUCHAR)d, n, 0);
        BCryptFinishHash(h, out, 16, 0);
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
}

static uint8_t *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(sz > 0 ? sz : 1);
    *len = fread(b, 1, sz, f);
    fclose(f);
    return b;
}

// USER_DATA010 record at 0x5400: a 0x40-byte head (mgid at +0x25), the run mid (tile at 0x5445, position at
// 0x5449, mgid at 0x546d), then the lobby record (len 8, lobby id, 01 00 00), which shifts the rest by 8.
#define REC 0x5400
// Solo save: insert the resume header and a rejoin record for the fake lobby, shifting the rest by 8 bytes.
static void plant_record(uint8_t *na, const uint8_t *a, uint32_t L, ULONG an, const uint8_t mg[8],
                         const uint8_t rec[15], const uint8_t box[20], bool have_box)
{
    static const uint8_t head[0x40] = {0, 0, 0, 0, 0, 0xea, 0x87, 0x62, 0x18, 0x43, 0, 0, 0, 0xea, 0x87, 0x62,
                                       0xc8, 0x6e, 0, 0, 0, 0, 5, 0, 0, 0xd, 0, 0, 0, 2};
    memcpy(na, a, 0x5400);
    memcpy(na + 0x5400, head, 0x40), memcpy(na + 0x5425, mg, 8);
    memcpy(na + 0x5440, a + 0x5440, 0x45), memcpy(na + 0x546d, mg, 8);
    static const uint8_t ff4[4] = {0xff, 0xff, 0xff, 0xff};
    if (!memcmp(na + 0x5445, ff4, 4) && have_box)
        memcpy(na + 0x5445, box, 20);
    memcpy(na + 0x5485, rec, 15);
    memcpy(na + 0x5494, a + 0x548c, 4 + L - 16 - 0x5494);
    memcpy(na + 4 + L, a + 4 + L, an - 4 - L);
}

static int plant_save(const char *path, uint64_t mgid, const uint8_t box[20], bool have_box)
{
    size_t n;
    uint8_t *s = read_all(path, &n);
    if (!s || n < 0x40)
        return free(s), -1;
    uint32_t cnt, off = 0, size = 0;
    uint64_t es;
    memcpy(&cnt, s + 0xc, 4), memcpy(&es, s + 0x20, 8);
    for (uint32_t i = 0; i < cnt && 0x40 + (i + 1) * es <= n; i++) {
        const uint8_t *e = s + 0x40 + i * es;
        uint32_t no;
        memcpy(&no, e + 0x14, 4);
        if (no + 26 <= n && !memcmp(s + no, L"USER_DATA010", 26)) {
            uint64_t sz;
            memcpy(&sz, e + 8, 8), memcpy(&off, e + 0x10, 4);
            size = (uint32_t)sz;
        }
    }
    if (!size || off + size > n || size < 0x10000)
        return free(s), -2;
    uint8_t iv[16], *a = s + off + 16, sum[16];
    ULONG an = size - 16;
    memcpy(iv, s + off, 16);
    if (!aes_cbc(false, a, an, iv))
        return free(s), -3;
    uint32_t L;
    memcpy(&L, a, 4);
    md5(a + 4, L - 16, sum);
    if (L + 4 > an || memcmp(sum, a + 4 + L - 16, 16))
        return free(s), -4;
    uint8_t rec[15] = {8, 0, 0, 0}, mg[8];
    memcpy(rec + 4, &fake_lobby, 8), rec[12] = 1;
    memcpy(mg, &mgid, 8);
    if (!memcmp(a + 0x5485, rec, 15)) {
        bool same = !memcmp(a + 0x546d, mg, 8);
        free(s);
        return same ? 1 : -5;
    }
    static const uint8_t z7[7];
    uint8_t *na = malloc(an);
    int ret = 0;
    if (!memcmp(a + 0x5485, rec, 4) && !memcmp(a + 0x546d, mg, 8)) {
        // A co-op save already holds the game's own rejoin record for this run: point it at the fake lobby.
        memcpy(na, a, an);
        memcpy(na + 0x5489, &fake_lobby, 8);
        ret = 2;
    } else if (memcmp(a + 0x5485, z7, 7)) {
        free(na);
        return free(s), -6;
    } else
        plant_record(na, a, L, an, mg, rec, box, have_box);
    md5(na + 4, L - 16, na + 4 + L - 16);
    memcpy(iv, s + off, 16);
    bool ok = aes_cbc(true, na, an, iv);
    if (ok)
        memcpy(a, na, an);
    free(na);
    char bak[MAX_PATH + 16];
    snprintf(bak, sizeof bak, "%s.pre_restore", path);
    if (!ok || !CopyFileA(path, bak, FALSE))
        return free(s), -7;
    FILE *f = fopen(path, "wb");
    size_t w = f ? fwrite(s, 1, n, f) : 0;
    if (f)
        fclose(f);
    free(s);
    return w == n ? ret : -8;
}

// Steam isn't up yet when the save must be planted, so the own SteamID is the save folder written last.
static uint64_t find_self(const char *appdata)
{
    char pat[MAX_PATH], save[MAX_PATH];
    snprintf(pat, MAX_PATH, "%s\\Nightreign\\7656119*", appdata ? appdata : "");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    uint64_t best = 0;
    FILETIME best_t = {0};
    do {
        WIN32_FILE_ATTRIBUTE_DATA a;
        snprintf(save, MAX_PATH, "%s\\Nightreign\\%.20s\\NR0000.sl2", appdata, fd.cFileName);
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !GetFileAttributesExA(save, GetFileExInfoStandard, &a))
            continue;
        if (CompareFileTime(&a.ftLastWriteTime, &best_t) > 0)
            best_t = a.ftLastWriteTime, best = strtoull(fd.cFileName, NULL, 10);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    note(best ? LOG_DEBUG : LOG_WARN, "Self from save folder %llu", (unsigned long long)best);
    return best;
}

static DWORD WINAPI auto_restore(LPVOID unused)
{
    (void)unused;
    char path[MAX_PATH], save[MAX_PATH];
    unsigned long long mgid = 0;
    // Mates are told apart from self, so this runs on every launch.
    const char *appdata = getenv("APPDATA");
    if (!fake_self)
        fake_self = find_self(appdata);
    snprintf(path, MAX_PATH, "%.*srun_active.txt", dll_dir_len, dll_dir);
    FILE *f = cfg("restore", "auto_restore", 1) ? fopen(path, "r") : NULL;
    if (!f)
        return 0;
    if (fscanf(f, "%llu", &mgid) != 1)
        mgid = 0;
    fclose(f);
    snprintf(path, MAX_PATH, "%.*sp2p_snapshot_run_%llu.bin", dll_dir_len, dll_dir, mgid);
    size_t n = 0;
    uint8_t *snapshot = mgid && fake_lobby ? read_all(path, &n) : NULL;
    if (!snapshot) {
        note(LOG_ERROR, "Auto restore: no snapshot for mgid=%llu", mgid);
        return 0;
    }
    run_mgid = mgid;
    snprintf(path, MAX_PATH, "%.*srun_mates.txt", dll_dir_len, dll_dir);
    unsigned long long mm = 0, m0 = 0, m1 = 0;
    if ((f = fopen(path, "r"))) {
        if (fscanf(f, "%llu %llu %llu", &mm, &m0, &m1) >= 2 && mm == mgid) {
            run_mates[0] = m0, run_mates[1] = m1;
            fake_host = m0;
            note(LOG_INFO, "Auto restore fake host = mate %llu", m0);
        }
        fclose(f);
    }
    uint64_t clock;
    uint8_t box[20];
    bool have_box = build_answers(snapshot, n, &clock, box);
    free(snapshot);
    snprintf(save, MAX_PATH, "%s\\Nightreign\\%llu\\NR0000.sl2", appdata ? appdata : "", (unsigned long long)fake_self);
    int r = plant_save(save, mgid, box, have_box);
    restore_mgid = mgid;
    note(r < 0 ? LOG_ERROR : LOG_INFO, "Auto restore mgid=%llu plant=%d box=%d", mgid, r, have_box);
    // rekindle_snapshot.dll reads restore_clock.txt once Steam is up, seconds after this thread has run.
    if (r >= 0 && clock) {
        snprintf(path, MAX_PATH, "%.*srestore_clock.txt", dll_dir_len, dll_dir);
        if ((f = fopen(path, "w")))
            fprintf(f, "%llu %llx\n", mgid, (unsigned long long)clock), fclose(f);
    }
    return 0;
}

static void emu_push_rel(const uint8_t *body, size_t blen);

// Answer records: u8 class (0x51 join kind, 0x42 character id), u32 key, u16 len, frame.
static void emu_answer(uint8_t cls, uint32_t key)
{
    int n = 0;
    for (size_t o = 0; emu_answers && o + 7 <= emu_answers_len;) {
        uint32_t k;
        uint16_t len;
        memcpy(&k, emu_answers + o + 1, 4);
        memcpy(&len, emu_answers + o + 5, 2);
        if (emu_answers[o] == cls && k == key)
            emu_push_rel(emu_answers + o + 7, len), n++;
        o += 7 + len;
    }
    note(LOG_DEBUG, "Emu answer %02x key=%08x frames=%d", cls, key, n);
}

// Reliable data and fragment frames get the host's next seq here; a fragment's first-seq field follows it.
static uint16_t emu_rseq = EMU_SEQ0 + 1, emu_frag_first;

static void emu_push_rel(const uint8_t *body, size_t blen)
{
    uint8_t b[1300];
    if (blen > sizeof b)
        return;
    memcpy(b, body, blen);
    int type = b[5] & 0x0f;
    if ((b[0] & 1) && (type == 3 || type == 7) && blen >= 7) {
        put_be16(b + 1, emu_rseq);
        if (type == 7 && blen >= 17) {
            if (!b[9] && !b[10] && !b[11] && !b[12])
                emu_frag_first = emu_rseq;
            put_be16(b + 7, emu_frag_first);
        }
        emu_rseq++;
    }
    emu_push_enc(b, blen);
}

static void swap_bytes(uint8_t *p, size_t n, const uint8_t *from, const uint8_t *to, size_t k)
{
    for (size_t i = 0; i + k <= n; i++)
        if (!memcmp(p + i, from, k))
            memcpy(p + i, to, k), i += k - 1;
}

// The host's own intro messages are the game's own, sent back with its SteamID swapped for the fake host's.
static const uint8_t emu_echo_types[] = {0xfa, 0x60, 0x81, 0x2a, 0x69, 0x6a};
static bool emu_echoed[256];

static void emu_echo(const uint8_t *msg, size_t n)
{
    if (emu_echoed[msg[0]] || !memchr(emu_echo_types, msg[0], sizeof emu_echo_types) || n + 7 > 1200)
        return;
    emu_echoed[msg[0]] = true;
    uint8_t b[1300] = {0x01, 0, 0, 0, 0, 0x03, 0xff};
    memcpy(b + 7, msg, n);
    uint8_t f[8], t[8], fs[24], ts[24], fw[48], tw[48];
    for (int i = 0; i < 8; i++)
        f[i] = (uint8_t)(fake_self >> 8 * i), t[i] = (uint8_t)(fake_host >> 8 * i);
    swap_bytes(b + 7, n, f, t, 8);
    for (int i = 0; i < 8; i++)
        f[i] = (uint8_t)(fake_self >> (56 - 8 * i)), t[i] = (uint8_t)(fake_host >> (56 - 8 * i));
    swap_bytes(b + 7, n, f, t, 8);
    int k = snprintf((char *)fs, sizeof fs, "%llu", (unsigned long long)fake_self);
    if (k == snprintf((char *)ts, sizeof ts, "%llu", (unsigned long long)fake_host)) {
        swap_bytes(b + 7, n, fs, ts, k);
        for (int i = 0; i < k; i++)
            fw[2 * i] = fs[i], tw[2 * i] = ts[i], fw[2 * i + 1] = tw[2 * i + 1] = 0;
        swap_bytes(b + 7, n, fw, tw, 2 * k);
    }
    emu_push_rel(b, n + 7);
    note(LOG_DEBUG, "Emu echo type=%02x len=%u", msg[0], (unsigned)n);
}

// Moves replay frames whose recorded time has come into the outgoing queue.
static void emu_pump(void)
{
    if (emu_state < 2)
        return;
    if (emu_stream && emu_stream_t0) {
        uint64_t st = unix_ms() - emu_stream_t0;
        while (emu_stream_pos + 6 <= emu_stream_len && emu_qtail - emu_qhead < 4000) {
            uint32_t t;
            uint16_t len;
            memcpy(&t, emu_stream + emu_stream_pos, 4);
            memcpy(&len, emu_stream + emu_stream_pos + 4, 2);
            if (t > st)
                break;
            emu_push_rel(emu_stream + emu_stream_pos + 6, len);
            emu_stream_pos += 6 + len;
            if (emu_stream_pos + 6 > emu_stream_len) {
                note(LOG_INFO, "Emu stream done");
                host_gone_at = unix_ms() + 5000;
            }
        }
    }
    if (!emu_replay)
        return;
    uint64_t now = unix_ms() - emu_t0;
    while (emu_replay_pos + 6 <= emu_replay_len && emu_qtail - emu_qhead < 4000) {
        uint32_t t;
        uint16_t len;
        memcpy(&t, emu_replay + emu_replay_pos, 4);
        memcpy(&len, emu_replay + emu_replay_pos + 4, 2);
        if (t > now)
            break;
        emu_push_rel(emu_replay + emu_replay_pos + 6, len);
        emu_replay_pos += 6 + len;
        if (emu_replay_pos + 6 > emu_replay_len)
            note(LOG_DEBUG, "Emu replay done");
    }
}

static void emu_on_frame(const uint8_t *f, size_t len)
{
    if (f[1] & 0x40) {
        uint32_t nonce = (uint32_t)f[2] << 24 | f[3] << 16 | f[4] << 8 | f[5];
        if (len == 14 && nonce && !emu_game_nonce) {
            emu_game_nonce = nonce;
            uint8_t p[28] = {0x0e, 0x40};
            p[14] = 0x0e, p[15] = 0x40;
            put_be32(p + 16, emu_host_nonce);
            emu_push(p + 14, 14);
            emu_push(p, 28);
            emu_state = 1;
            note(LOG_DEBUG, "Emu nonce game=%08x host=%08x", emu_game_nonce, emu_host_nonce);
        }
        return;
    }
    if (!(f[1] & 0x80) || len < 8 + 7 || !emu_game_nonce)
        return;
    uint8_t b[2048];
    size_t blen = len - 4;
    memcpy(b, f + 4, blen);
    emu_ctr(b, blen, get_be16(f + 2));
    uint32_t crc;
    memcpy(&crc, b, 4);
    const uint8_t *body = b + 4;
    blen -= 4;
    if (crc != emu_crc32(body, blen)) {
        note(LOG_WARN, "Emu bad crc");
        return;
    }
    if (!(body[0] & 1))
        return;
    uint16_t seq = get_be16(body + 1);
    int type = body[5] & 0x0f;
    if (type == 1) {
        if (emu_state < 2) {
            emu_push_hello(0);
            emu_push_hello(seq);
            emu_state = 2;
            emu_t0 = unix_ms();
            note(LOG_DEBUG, "Emu hello game seq=%04x", seq);
        }
    } else if (type == 3 || type == 7) {
        uint8_t ack[7] = {0x01, 0, 0, 0, 0, 0x30, 0xff};
        put_be16(ack + 3, seq);
        emu_push_enc(ack, sizeof ack);
        if (blen > 7)
            note(LOG_DEBUG, "Emu in type=%02x len=%u", type == 3 ? body[7] : body[17], (unsigned)blen - 7);
        if (type == 3 && blen > 7)
            emu_echo(body + 7, blen - 7);
        if (type == 3 && blen >= 9 && blen <= 7 + 16 && (body[7] == 0x42 || body[7] == 0x51)) {
            char hex[40];
            for (size_t i = 0; i < blen - 7; i++)
                snprintf(hex + 2 * i, 3, "%02x", body[7 + i]);
            note(LOG_DEBUG, "Emu req +%llu %s", (unsigned long long)(unix_ms() - emu_t0), hex);
            if (body[7] == 0x51 && blen == 9)
                emu_answer(0x51, body[8]);
            else if (body[7] == 0x42 && blen == 16 && body[8] <= 1) {
                uint32_t id;
                memcpy(&id, body + 12, 4);
                emu_answer(0x42, id);
                if (!emu_stream_t0)
                    emu_stream_t0 = unix_ms();
            }
        }
    }
}

// Auth with the fake host: BeginAuthSession (ISteamUser slot 14) answers OK, and the next RunCallbacks
// delivers ValidateAuthTicketResponse_t (143: steam id, response u32 at 8, owner id at 12) to the game.
static void *cb143, *cb506;
static volatile LONG auth_pending;
static void **user_orig;
typedef int (*BeginAuth_t)(void *, const void *, int, uint64_t);
typedef void (*EndAuth_t)(void *, uint64_t);
typedef void (*RunCbs_t)(void);
static RunCbs_t real_run_cbs;

// Once the fake host has left, its id may belong to the real teammate rejoining: pass everything through.
#define EMU_ID(id) ((id) == fake_host && !host_gone)

static int us_begin(void *self, const void *ticket, int len, uint64_t id)
{
    if (!EMU_ID(id))
        return ((BeginAuth_t)user_orig[14])(self, ticket, len, id);
    InterlockedExchange(&auth_pending, 1);
    note(LOG_DEBUG, "Emu BeginAuthSession len=%d", len);
    return 0;
}

static void us_end(void *self, uint64_t id)
{
    if (!EMU_ID(id))
        ((EndAuth_t)user_orig[15])(self, id);
}

static void hook_run_cbs(void)
{
    real_run_cbs();
    if (cb143 && InterlockedExchange(&auth_pending, 0)) {
        uint8_t param[24] = {0};
        memcpy(param, &fake_host, 8);
        memcpy(param + 12, &fake_host, 8);
        note(LOG_DEBUG, "Emu deliver 143");
        ((Run_t)(*(void ***)cb143)[1])(cb143, param, 0, 0);
    }
    // LobbyChatUpdate_t (506): lobby, user changed, user making change, state u32 (2 = left).
    if (cb506 && host_gone_at && unix_ms() >= host_gone_at && !InterlockedExchange(&host_gone, 1)) {
        uint64_t param[4] = {fake_lobby, fake_host, fake_host, 2};
        note(LOG_INFO, "Emu host left lobby");
        ((Run_t)(*(void ***)cb506)[1])(cb506, param, 0, 0);
        if (emu_mate2) {
            uint64_t left2[4] = {fake_lobby, emu_mate2, emu_mate2, 2};
            note(LOG_INFO, "Emu second mate %llu left lobby", (unsigned long long)emu_mate2);
            ((Run_t)(*(void ***)cb506)[1])(cb506, left2, 0, 0);
        }
    }
    relay_tick();
}

typedef bool (*P2PSend_t)(void *, uint64_t, const void *, uint32_t, int, int);
typedef bool (*P2PRead_t)(void *, void *, uint32_t, uint32_t *, uint64_t *, int);
typedef bool (*P2PAvail_t)(void *, uint32_t *, int);
typedef bool (*P2PUser_t)(void *, uint64_t);
typedef bool (*P2PChan_t)(void *, uint64_t, int);
typedef bool (*P2PState_t)(void *, uint64_t, uint8_t *);

static bool nk_send(void *self, uint64_t id, const void *data, uint32_t len, int type, int channel)
{
    if (!EMU_ID(id))
        return ((P2PSend_t)net_orig[0])(self, id, data, len, type, channel);
    EnterCriticalSection(&emu_lock);
    const uint8_t *p = data;
    for (uint32_t i = 0; i + 2 <= len;) {
        uint32_t fl = (p[i + 1] & 7) << 8 | p[i];
        if (!fl || i + fl > len)
            break;
        emu_on_frame(p + i, fl);
        i += fl;
    }
    LeaveCriticalSection(&emu_lock);
    return true;
}

static bool nk_avail(void *self, uint32_t *size, int channel)
{
    if (channel == EMU_CHANNEL && fake_host && !host_gone) {
        EnterCriticalSection(&emu_lock);
        emu_pump();
        bool have = emu_qhead != emu_qtail;
        if (have && size)
            *size = emu_q[emu_qhead % 4096].len;
        LeaveCriticalSection(&emu_lock);
        if (have)
            return true;
    }
    return ((P2PAvail_t)net_orig[1])(self, size, channel);
}

static bool nk_read(void *self, void *dest, uint32_t cap, uint32_t *size, uint64_t *remote, int channel)
{
    if (channel == EMU_CHANNEL && fake_host && !host_gone) {
        EnterCriticalSection(&emu_lock);
        emu_pump();
        bool have = emu_qhead != emu_qtail;
        if (have) {
            uint32_t n = emu_q[emu_qhead % 4096].len;
            memcpy(dest, emu_q[emu_qhead % 4096].data, n < cap ? n : cap);
            if (size)
                *size = n;
            if (remote)
                *remote = fake_host;
            emu_qhead++;
        }
        LeaveCriticalSection(&emu_lock);
        if (have)
            return true;
    }
    return ((P2PRead_t)net_orig[2])(self, dest, cap, size, remote, channel);
}

static bool nk_accept(void *self, uint64_t id)
{
    return EMU_ID(id) || ((P2PUser_t)net_orig[3])(self, id);
}

static bool nk_close(void *self, uint64_t id)
{
    if (!EMU_ID(id))
        return ((P2PUser_t)net_orig[4])(self, id);
    note(LOG_DEBUG, "Emu CloseP2PSessionWithUser");
    return true;
}

static bool nk_close_chan(void *self, uint64_t id, int channel)
{
    if (!EMU_ID(id))
        return ((P2PChan_t)net_orig[5])(self, id, channel);
    note(LOG_DEBUG, "Emu CloseP2PChannelWithUser %d", channel);
    return true;
}

// P2PSessionState_t: active, connecting, error, relay bytes, then queue counters and remote address.
static bool nk_state(void *self, uint64_t id, uint8_t *st)
{
    if (!EMU_ID(id))
        return ((P2PState_t)net_orig[6])(self, id, st);
    memset(st, 0, 20);
    st[0] = 1, st[3] = 1;
    note(LOG_DEBUG, "Emu GetP2PSessionState");
    return true;
}

static void install_fakes(int kind, void **vt, void **nv, int slots)
{
    if (!fake_lobby)
        return;
    if (kind == 1 && slots > 37) {
        mm_orig = vt;
        memcpy(mm_stub, nv, 38 * sizeof *nv);
        static void *const tr_fns[][2] = {
            {(void *)13, tr13}, {(void *)15, tr15}, {(void *)16, tr16}, {(void *)19, tr19}, {(void *)20, tr20}, {(void *)21, tr21},
            {(void *)23, tr23}, {(void *)24, tr24}, {(void *)25, tr25}, {(void *)26, tr26}, {(void *)28, tr28},
            {(void *)31, tr31}, {(void *)32, tr32}, {(void *)33, tr33}, {(void *)34, tr34}, {(void *)36, tr36},
            {(void *)37, tr37}};
        for (size_t i = 0; i < sizeof tr_fns / sizeof *tr_fns; i++)
            nv[(uintptr_t)tr_fns[i][0]] = tr_fns[i][1];
        nv[14] = fk_join;
        nv[17] = fk_num;
        nv[18] = fk_member;
        nv[35] = fk_owner;
    } else if (kind == 4 && slots > 13) {
        utils_orig = vt;
        nv[11] = fk_done;
        nv[13] = fk_result;
    } else if (kind == 5 && slots > 15) {
        user_orig = vt;
        nv[14] = us_begin;
        nv[15] = us_end;
    } else if (kind == 6 && slots > 6) {
        net_orig = vt;
        nv[0] = nk_send;
        nv[1] = nk_avail;
        nv[2] = nk_read;
        nv[3] = nk_accept;
        nv[4] = nk_close;
        nv[5] = nk_close_chan;
        nv[6] = nk_state;
        EnterCriticalSection(&emu_lock);
        if (!emu_key) {
            emu_init();
            note(LOG_DEBUG, "Emu ready answers=%d", emu_built);
        }
        LeaveCriticalSection(&emu_lock);
    }
}

static void wrap_iface(void *obj, const char *ver)
{
    int kind = !strncmp(ver, "SteamMatchMaking", 16) ? 1 : !strncmp(ver, "SteamFriends", 12) ? 2
             : !strncmp(ver, "SteamClient", 11)      ? 3 : !strncmp(ver, "SteamUtils", 10) ? 4
             : !strncmp(ver, "SteamUser", 9) && strncmp(ver, "SteamUserStats", 14) ? 5
             : !strncmp(ver, "SteamNetworking0", 16) ? 6 : 0;
    if (!kind || !readable(obj, 8))
        return;
    EnterCriticalSection(&lock);
    void **vt = *(void ***)obj;
    int k;
    for (k = 0; k < iface_count; k++)
        if (ifaces[k].wrapped == vt || ifaces[k].orig == vt)
            break;
    if (k == iface_count && k < 16) {
        int slots = kind == 1 ? 44 : kind == 2 ? 100 : 40;
        while (slots && !readable(vt, slots * 8))
            slots--;
        void **nv = VirtualAlloc(NULL, slots * 8 + slots * 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        uint8_t *code = (uint8_t *)(nv + slots);
        for (int s = 0; s < slots; s++) {
            // Matchmaking methods with stack arguments, and every friends method, log before the call only.
            int pre = kind == 2 || kind >= 4 ||
                      (kind == 1 && (s == 1 || s == 2 || s == 3 || s == 22 || s == 27 || s == 29 || s == 30)) ||
                      (kind == 3 && s != 5 && s != 8 && s != 9 && s != 10 && s != 12 && s != 16);
            uint8_t *t = code + s * 32;
            uint64_t key = ((uint64_t)k << 16) | s, h = (uint64_t)(pre ? steam_thunk_pre : steam_thunk_post);
            t[0] = 0x49, t[1] = 0xba, memcpy(t + 2, &key, 8);
            t[10] = 0x49, t[11] = 0xbb, memcpy(t + 12, &h, 8);
            t[20] = 0x41, t[21] = 0xff, t[22] = 0xe3;
            nv[s] = t;
        }
        install_fakes(kind, vt, nv, slots);
        if (kind == 1)
            mm_self = obj;
        else if (kind == 4)
            utils_self = obj;
        ifaces[k].name = _strdup(ver);
        ifaces[k].orig = vt;
        ifaces[k].wrapped = nv;
        steam_orig_tables[k] = vt;
        iface_count++;
        char info[160];
        snprintf(info, sizeof info, "%s obj=%p vtable=%p slots=%d", ver, obj, (void *)vt, slots);
        LeaveCriticalSection(&lock);
        log_event("SteamWrap", NULL, info, NULL, 0);
        EnterCriticalSection(&lock);
    }
    if (k < iface_count && vt == ifaces[k].orig)
        *(void ***)obj = ifaces[k].wrapped;
    LeaveCriticalSection(&lock);
}

typedef void *(*FindIface_t)(int32_t, const char *);
typedef void *(*CreateIface_t)(const char *);
typedef void (*RegCb_t)(void *, int);
typedef void (*UnregCb_t)(void *);
typedef void (*RegCr_t)(void *, uint64_t);
static FindIface_t real_find_iface;
static CreateIface_t real_create_iface;
static RegCb_t real_reg_cb;
static UnregCb_t real_unreg_cb;
static RegCr_t real_reg_cr, real_unreg_cr;

static void *hook_find_iface(int32_t user, const char *ver)
{
    void *p = real_find_iface(user, ver);
    wrap_iface(p, ver ? ver : "");
    return p;
}

static void *hook_create_iface(const char *ver)
{
    void *p = real_create_iface(ver);
    wrap_iface(p, ver ? ver : "");
    return p;
}

// Callback objects get a private vtable copy whose Run slots log; slot -1 holds the original vtable.
static void log_cb(void *self, int slot, void *param, uint64_t b, uint64_t c)
{
    if (log_level < LOG_DEBUG)
        return;
    void **orig = (*(void ***)self)[-1];
    int id = *(int *)((uint8_t *)self + 0xc), size = ((int (*)(void *))orig[2])(self);
    char line[1400];
    int n = snprintf(line, sizeof line, "cb=%p id=%d run%d size=%d b=%llu c=0x%llx data=", self, id, slot, size,
                     (unsigned long long)(b & 0xff), (unsigned long long)c);
    for (int i = 0; i < size && i < 256 && readable((uint8_t *)param + i, 1); i++)
        n += snprintf(line + n, sizeof line - n, "%02x", ((uint8_t *)param)[i]);
    log_dedup("SteamCallback", line);
}
// Lobby callbacks (504 to 516) show the real lobby as the game's id; 505 holds it twice.
static void cb_relay(void *self, void *p)
{
    int id = *(int *)((uint8_t *)self + 0xc);
    if (!real_lobby || id < 504 || id > 516)
        return;
    int size = ((int (*)(void *))((void **)(*(void ***)self)[-1])[2])(self);
    for (int o = 0; o + 8 <= size; o += 8)
        if (*(uint64_t *)((uint8_t *)p + o) == real_lobby)
            *(uint64_t *)((uint8_t *)p + o) = game_lobby;
}
static void cb_run0(void *self, void *p, uint64_t b, uint64_t c)
{
    cb_relay(self, p);
    log_cb(self, 0, p, b, c);
    ((Run_t)((void **)(*(void ***)self)[-1])[0])(self, p, b, c);
}
static void cb_run1(void *self, void *p, uint64_t b, uint64_t c)
{
    // LobbyDataUpdate_t (505): lobby u64, member u64, success byte at 16.
    if (fake_lobby && *(int *)((uint8_t *)self + 0xc) == 505 && *(uint64_t *)p == fake_lobby &&
        !((uint8_t *)p)[16]) {
        ((uint8_t *)p)[16] = 1;
        note(LOG_DEBUG, "505 success forced");
    }
    cb_relay(self, p);
    if (*(int *)((uint8_t *)self + 0xc) == 506 && ((uint32_t *)p)[6] & 1)
        note_mate(((uint64_t *)p)[1]);
    log_cb(self, 1, p, b, c);
    ((Run_t)((void **)(*(void ***)self)[-1])[1])(self, p, b, c);
}

static void wrap_cb(void *cb)
{
    static void **orig[256], **wrapped[256];
    static int count;
    EnterCriticalSection(&lock);
    void **vt = *(void ***)cb;
    int i;
    for (i = 0; i < count; i++)
        if (orig[i] == vt || wrapped[i] == vt)
            break;
    if (i == count && count < 256 && readable(vt, 8 * 8)) {
        void **nv = malloc(9 * 8);
        nv[0] = orig[count] = vt;
        memcpy(nv + 1, vt, 8 * 8);
        nv[1] = cb_run0;
        nv[2] = cb_run1;
        wrapped[count++] = nv + 1;
    }
    if (i < count && orig[i] == vt)
        *(void ***)cb = wrapped[i];
    LeaveCriticalSection(&lock);
}

static void hook_reg_cb(void *cb, int id)
{
    char info[64];
    snprintf(info, sizeof info, "cb=%p id=%d", cb, id);
    log_event("RegisterCallback", NULL, info, NULL, 0);
    if (id == 143)
        cb143 = cb;
    else if (id == 506)
        cb506 = cb;
    wrap_cb(cb);
    real_reg_cb(cb, id);
}

static void hook_unreg_cb(void *cb)
{
    char info[64];
    snprintf(info, sizeof info, "cb=%p id=%d", cb, *(int *)((uint8_t *)cb + 0xc));
    log_dedup("UnregisterCallback", info);
    real_unreg_cb(cb);
}

static void hook_reg_cr(void *cb, uint64_t call)
{
    char info[96];
    snprintf(info, sizeof info, "cb=%p id=%d call=0x%llx", cb, *(int *)((uint8_t *)cb + 0xc),
             (unsigned long long)call);
    log_event("RegisterCallResult", NULL, info, NULL, 0);
    wrap_cb(cb);
    real_reg_cr(cb, call);
}

static void hook_unreg_cr(void *cb, uint64_t call)
{
    char info[96];
    snprintf(info, sizeof info, "cb=%p call=0x%llx", cb, (unsigned long long)call);
    log_event("UnregisterCallResult", NULL, info, NULL, 0);
    real_unreg_cr(cb, call);
}

// ---- EOS stubs: in local mode the game's EOS delay imports point here, so the SDK and EAC never load ----

#define EOS_FAKE ((void *)0xdeadbeef)
typedef void (*EosCb_t)(void *info);
static void *eos_auth_data;
static EosCb_t eos_auth_cb;

static int eos_ok(void) { return 0; }
static void eos_nothing(void) {}
static void *eos_fake(void) { return EOS_FAKE; }
static int eos_true(void) { return 1; }
static int eos_logged_in(void) { return 2; }
static int eos_not_found(void) { return 18; }

static int eos_puid_to_string(void *id, char *out, int32_t *len)
{
    char s[24];
    int n = snprintf(s, sizeof s, "%llu", (unsigned long long)fake_self);
    if (*len < n + 1)
        return 17;
    memcpy(out, s, n + 1);
    *len = n + 1;
    return 0;
}

static int eos_bytes_to_string(const uint8_t *p, uint32_t n, char *out, uint32_t *len)
{
    if (n + 1 <= *len)
        memcpy(out, p, n), out[n] = 0;
    *len = n + 1;
    return 0;
}

static void eos_login(void *h, void *opts, void *data, EosCb_t cb)
{
    struct { int32_t result; void *data, *user, *token; } info = {0, data, EOS_FAKE, NULL};
    log_event("EOS", NULL, "Connect_Login", NULL, 0);
    cb(&info);
}

static void *eos_add_auth_notify(void *h, void *opts, void *data, EosCb_t cb)
{
    eos_auth_data = data, eos_auth_cb = cb;
    return EOS_FAKE;
}

static int eos_register_peer(void *h, const uint8_t *opts)
{
    struct { void *data, *peer; int32_t status; } info = {eos_auth_data, *(void **)(opts + 8), 2};
    if (eos_auth_cb)
        eos_auth_cb(&info);
    return 0;
}

// Points every EOSSDK delay-import slot at a stub; returns how many were patched.
static int patch_eos(void)
{
    static const struct { const char *name; void *fn; } stubs[] = {
        {"EOS_ByteArray_ToString", eos_bytes_to_string},
        {"EOS_ProductUserId_IsValid", eos_true},
        {"EOS_ProductUserId_ToString", eos_puid_to_string},
        {"EOS_ProductUserId_FromString", eos_fake},
        {"EOS_Initialize", eos_ok},
        {"EOS_Shutdown", eos_ok},
        {"EOS_Platform_Create", eos_fake},
        {"EOS_Platform_Release", eos_nothing},
        {"EOS_Platform_Tick", eos_nothing},
        {"EOS_Platform_GetConnectInterface", eos_fake},
        {"EOS_Platform_GetAntiCheatClientInterface", eos_fake},
        {"EOS_Logging_SetCallback", eos_ok},
        {"EOS_Logging_SetLogLevel", eos_ok},
        {"EOS_Connect_Login", eos_login},
        {"EOS_Connect_CreateUser", eos_nothing},
        {"EOS_Connect_GetLoginStatus", eos_logged_in},
        {"EOS_Connect_AddNotifyAuthExpiration", eos_fake},
        {"EOS_Connect_RemoveNotifyAuthExpiration", eos_nothing},
        {"EOS_Connect_AddNotifyLoginStatusChanged", eos_fake},
        {"EOS_Connect_RemoveNotifyLoginStatusChanged", eos_nothing},
        {"EOS_AntiCheatClient_AddNotifyMessageToPeer", eos_fake},
        {"EOS_AntiCheatClient_RemoveNotifyMessageToPeer", eos_nothing},
        {"EOS_AntiCheatClient_AddNotifyPeerActionRequired", eos_fake},
        {"EOS_AntiCheatClient_RemoveNotifyPeerActionRequired", eos_nothing},
        {"EOS_AntiCheatClient_AddNotifyPeerAuthStatusChanged", eos_add_auth_notify},
        {"EOS_AntiCheatClient_RemoveNotifyPeerAuthStatusChanged", eos_nothing},
        {"EOS_AntiCheatClient_BeginSession", eos_ok},
        {"EOS_AntiCheatClient_EndSession", eos_ok},
        {"EOS_AntiCheatClient_PollStatus", eos_not_found},
        {"EOS_AntiCheatClient_RegisterPeer", eos_register_peer},
        {"EOS_AntiCheatClient_ReceiveMessageFromPeer", eos_ok},
        {"EOS_AntiCheatClient_UnregisterPeer", eos_ok},
    };
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    int patched = 0, total = 0;
    for (IMAGE_DELAYLOAD_DESCRIPTOR *d = (void *)(base + dir.VirtualAddress); dir.VirtualAddress && d->DllNameRVA; d++) {
        if (_stricmp((char *)base + d->DllNameRVA, "EOSSDK-Win64-Shipping.dll"))
            continue;
        IMAGE_THUNK_DATA *names = (void *)(base + d->ImportNameTableRVA);
        uintptr_t *slots = (void *)(base + d->ImportAddressTableRVA);
        for (; names->u1.AddressOfData; names++, slots++, total++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            const char *fn = ((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
            for (size_t i = 0; i < sizeof stubs / sizeof *stubs; i++) {
                if (strcmp(fn, stubs[i].name))
                    continue;
                DWORD old;
                VirtualProtect(slots, 8, PAGE_READWRITE, &old);
                *slots = (uintptr_t)stubs[i].fn;
                VirtualProtect(slots, 8, old, &old);
                patched++;
            }
        }
    }
    log_line(patched ? LOG_INFO : LOG_ERROR, "Local mode: EOS stubs patched=%d of %d", patched, total);
    return patched;
}

static void patch_imports(void)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    static const struct {
        const char *name;
        void *hook;
        void **real;
    } table[] = {
        {"SteamInternal_FindOrCreateUserInterface", hook_find_iface, (void **)&real_find_iface},
        {"SteamInternal_CreateInterface", hook_create_iface, (void **)&real_create_iface},
        {"SteamAPI_RegisterCallback", hook_reg_cb, (void **)&real_reg_cb},
        {"SteamAPI_RunCallbacks", hook_run_cbs, (void **)&real_run_cbs},
        {"SteamAPI_UnregisterCallback", hook_unreg_cb, (void **)&real_unreg_cb},
        {"SteamAPI_RegisterCallResult", hook_reg_cr, (void **)&real_reg_cr},
        {"SteamAPI_UnregisterCallResult", hook_unreg_cr, (void **)&real_unreg_cr},
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
    int patched = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(base + dir.VirtualAddress); dir.VirtualAddress && d->Name; d++) {
        if (_stricmp((char *)base + d->Name, "winhttp.dll") && _stricmp((char *)base + d->Name, "steam_api64.dll"))
            continue;
        IMAGE_THUNK_DATA *names = (void *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *slots = (void *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            const char *fn = ((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
            for (size_t i = 0; i < sizeof table / sizeof *table; i++) {
                if (strcmp(fn, table[i].name))
                    continue;
                DWORD old;
                VirtualProtect(&slots->u1.Function, 8, PAGE_READWRITE, &old);
                *table[i].real = (void *)(uintptr_t)slots->u1.Function;
                slots->u1.Function = (uintptr_t)table[i].hook;
                VirtualProtect(&slots->u1.Function, 8, old, &old);
                patched++;
            }
        }
    }
    log_line(patched ? LOG_DEBUG : LOG_ERROR, "Import hooks patched=%d of %d", patched,
             (int)(sizeof table / sizeof *table));
}

// Logs fatal-looking exceptions with exe RVAs found on the stack, since the game exits without a dialog.
static LONG CALLBACK crash_veh(EXCEPTION_POINTERS *ep)
{
    static volatile LONG count;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != 0xc0000374 &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;
    if (log_level < LOG_ERROR || InterlockedIncrement(&count) > 20)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uintptr_t size = ((IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew))->OptionalHeader.SizeOfImage;
    CONTEXT *c = ep->ContextRecord;
    uintptr_t ip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    char line[2048];
    int n = snprintf(line, sizeof line, "CRASH code=%08lx ip=%p rva=0x%llx addr=%p rcx=%llx rdx=%llx rax=%llx", code,
                     (void *)ip, (unsigned long long)(ip - base < size ? ip - base : 0),
                     (void *)ep->ExceptionRecord->ExceptionInformation[1], (unsigned long long)c->Rcx,
                     (unsigned long long)c->Rdx, (unsigned long long)c->Rax);
#define ADD(...) (n += n < (int)sizeof line ? snprintf(line + n, sizeof line - n, __VA_ARGS__) : 0)
    // Real call chain via unwind data, then the regs that usually hold `this`, with their vtable rva.
    CONTEXT u = *c;
    ADD(" unwind:");
    for (int i = 0; i < 24 && u.Rip; i++) {
        DWORD64 ib;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(u.Rip, &ib, NULL);
        ADD(" 0x%llx", (unsigned long long)(u.Rip - base < size ? u.Rip - base : u.Rip));
        if (!fn)
            break;
        void *hd;
        DWORD64 ef;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, u.Rip, fn, &u, &hd, &ef, NULL);
    }
    DWORD64 regs[] = {c->Rbx, c->Rsi, c->Rdi, c->R8, c->R12, c->R13, c->R14, c->R15};
    ADD(" regs:");
    for (int i = 0; i < 8; i++) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t vt = 0;
        if (regs[i] > 0x10000 && VirtualQuery((void *)regs[i], &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            vt = *(uintptr_t *)regs[i];
        ADD(" %llx/%llx", (unsigned long long)regs[i], (unsigned long long)(vt - base < size ? vt - base : 0));
    }
    ADD(" stack:");
    uintptr_t *sp = (uintptr_t *)c->Rsp;
    for (int i = 0, k = 0; i < 0x300 && k < 24; i++) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!(i & 0x1ff) && (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT))
            break;
        if (sp[i] - base < size)
            ADD(" 0x%llx", (unsigned long long)(sp[i] - base)), k++;
    }
#undef ADD
    log_line(LOG_ERROR, "%s", line);
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID r)
{
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(self);
    InitializeCriticalSection(&lock);
    InitializeCriticalSection(&emu_lock);
    t0 = unix_ms();
    char path[MAX_PATH], v[16];
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    while (n && path[n - 1] != '\\' && path[n - 1] != '/')
        n--;
    memcpy(dll_dir, path, n);
    dll_dir_len = (int)n;
    snprintf(path + n, MAX_PATH - n, "rekindle.ini");
    GetPrivateProfileStringA("log", "level", "error", v, sizeof v, path);
    for (int i = 0; i < 5; i++)
        if (!_stricmp(v, level_names[i]))
            log_level = i;
    // The log only holds this launch: an old one is removed even when nothing gets written.
    snprintf(log_path, MAX_PATH, "%.*srekindle.log", (int)n, path);
    DeleteFileA(log_path);
    fake_lobby = DEFAULT_LOBBY, fake_host = DEFAULT_HOST;
    CloseHandle(CreateThread(NULL, 0, auto_restore, NULL, 0, NULL));
    AddVectoredExceptionHandler(1, crash_veh);
    CloseHandle(CreateThread(NULL, 0, startup_patches, NULL, 0, NULL));
    patch_imports();
    // Local mode needs this DLL loaded early, before the game's EOS setup runs.
    if (!GetModuleHandleA("cl_server_redirector.dll"))
        patch_eos();
    return TRUE;
}
