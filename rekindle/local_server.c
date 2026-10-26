// Local server: without cl_server_redirector every WinHttpConnect goes to this loopback server, which answers
// like the real one, so no FromSoftware server is ever contacted.
#include <winsock2.h>
#include "rekindle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile LONG local_port;
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
bool local_mode(void)
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
