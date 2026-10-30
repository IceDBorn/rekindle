// Ping and packet loss next to teammate names. Rekindle peers echo small pings on their own P2P channel;
// loss comes from gaps in the plaintext wire seq of the game's encrypted frames on channel 30.
#include "rekindle.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "game.h"
#include "patch.h"

#define PING_CHANNEL 0x524b
#define GAME_CHANNEL 30
#define PING_FAST_MS 500
#define PING_PROBE_MS 5000
#define PONG_TTL_MS 10000
#define PEER_TTL_MS 10000
#define LOSS_WINDOW_MS 2000
#define NAME_REFRESH_MS 5000
#define TEXT_REFRESH_MS 2000
#define MAX_PEERS 5
#define SET_UI_TEXT_RVA 0x792c70

struct peer {
    uint64_t id, seen_ms, next_ping_ms, pong_ms, name_ms, text_ms;
    uint32_t rtt[5], rtt_n, ping_seq, got, loss_tenths;
    uint16_t seq_next, seq_start;
    bool seq_on;
    wchar_t name[64], game_name[18], extra[32];
    uint32_t pid;
};

// Ping packet: "RKP", type (1 ping, 2 pong), u32 seq, u64 sender's clock in ms echoed back unchanged, and the
// sender's in-game name, which the labels show and which can differ from the Steam name.
struct ping_msg {
    char magic[3];
    uint8_t type;
    uint32_t seq;
    uint64_t t;
    wchar_t name[18];
    uint32_t pid;
};

void *friends_self, **friends_orig;
static struct peer peers[MAX_PEERS];
static SRWLOCK peers_lock = SRWLOCK_INIT;
static void *volatile net_self;
static void **net_vt;
static bool show_ping = true, show_loss = true, hud_on = true, over_on;
static double qpc_per_ms = 1;

typedef bool (*Send_t)(void *, uint64_t, const void *, uint32_t, int, int);
typedef bool (*Avail_t)(void *, uint32_t *, int);
typedef bool (*Read_t)(void *, void *, uint32_t, uint32_t *, uint64_t *, int);
typedef const char *(*PersonaName_t)(void *, uint64_t);
typedef void (*SetUiText_t)(void *, const wchar_t *);
static SetUiText_t real_set_ui_text;

static uint64_t now_ms(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / qpc_per_ms);
}

// Caller holds peers_lock exclusive. Reuses an expired slot for a new id.
static struct peer *peer_get(uint64_t id, uint64_t now)
{
    struct peer *slot = NULL;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peers[i].id == id)
            return &peers[i];
        if (!slot && (!peers[i].id || now - peers[i].seen_ms > PEER_TTL_MS))
            slot = &peers[i];
    }
    if (slot)
        memset(slot, 0, sizeof *slot), slot->id = id, slot->name_ms = now - NAME_REFRESH_MS;
    return slot;
}

static void count_seq(struct peer *p, uint16_t s)
{
    uint16_t ahead = (uint16_t)(s - p->seq_next), behind = (uint16_t)(p->seq_next - s);
    if (!p->seq_on || (ahead > 1000 && behind > 1000))
        p->seq_on = true, p->seq_start = s, p->seq_next = s + 1, p->got = 1;
    else if (ahead <= 1000)
        p->seq_next = s + 1, p->got++;
    else
        p->got++;
}

// Called from the networking wrappers for every packet the game sends to or reads from a real peer.
void ping_seen(void *self, void **vt, uint64_t id, const uint8_t *d, uint32_t n, int channel, bool incoming)
{
    if (!net_self)
        net_vt = vt, net_self = self;
    if (!id || (id == fake_host && run_restored && !host_gone) || channel == PING_CHANNEL)
        return;
    if (log_level >= LOG_DEBUG) {
        char line[64];
        snprintf(line, sizeof line, "ch=%d in=%d id=%llx", channel, incoming, (unsigned long long)id);
        log_dedup("P2P", line);
    }
    uint64_t now = now_ms();
    AcquireSRWLockExclusive(&peers_lock);
    struct peer *p = peer_get(id, now);
    if (p) {
        p->seen_ms = now;
        // Frames: u16 LE length in the low 11 bits; 0x80 in byte 1 marks an encrypted frame with a BE16 wire seq.
        for (uint32_t i = 0; incoming && channel == GAME_CHANNEL && d && i + 4 <= n;) {
            uint32_t fl = (d[i + 1] & 7) << 8 | d[i];
            if (!fl || i + fl > n)
                break;
            if (d[i + 1] & 0x80)
                count_seq(p, get_be16(d + i + 2));
            i += fl;
        }
    }
    ReleaseSRWLockExclusive(&peers_lock);
}

// PlayerGameData +0x9c holds the local player's in-game name, 16 characters at most.
static void send_msg(uint64_t id, uint8_t type, uint32_t seq, uint64_t t)
{
    struct ping_msg m = {{'R', 'K', 'P'}, type, seq, t, {0}, self_pid};
    uint8_t *gdm = *(uint8_t **)((uint8_t *)GetModuleHandleA(NULL) + GAME_DATA_MAN_RVA);
    uint8_t *pgd = gdm ? *(uint8_t **)(gdm + 8) : NULL;
    if (pgd)
        memcpy(m.name, pgd + 0x9c, 16 * sizeof(wchar_t));
    ((Send_t)net_vt[0])(net_self, id, &m, sizeof m, 0, PING_CHANNEL);
}

static void read_pings(uint64_t now)
{
    uint32_t size;
    struct ping_msg m;
    uint64_t from;
    bool pid_learned = false;
    for (int k = 0; k < 64 && ((Avail_t)net_vt[1])(net_self, &size, PING_CHANNEL); k++) {
        if (!((Read_t)net_vt[2])(net_self, &m, sizeof m, &size, &from, PING_CHANNEL))
            break;
        // Older builds send no player_id.
        if ((size != sizeof m && size != offsetof(struct ping_msg, pid)) || memcmp(m.magic, "RKP", 3))
            continue;
        if (size != sizeof m)
            m.pid = 0;
        if (m.type == 1)
            send_msg(from, 2, m.seq, m.t);
        m.name[17] = 0;
        AcquireSRWLockExclusive(&peers_lock);
        for (int i = 0; i < MAX_PEERS; i++) {
            if (peers[i].id != from)
                continue;
            memcpy(peers[i].game_name, m.name, sizeof m.name);
            if (m.pid && m.pid != peers[i].pid)
                peers[i].pid = m.pid, pid_learned = true;
            if (m.type == 2 && now >= m.t && now - m.t < 5000) {
                peers[i].rtt[peers[i].rtt_n++ % 5] = (uint32_t)(now - m.t);
                peers[i].pong_ms = now;
            }
        }
        ReleaseSRWLockExclusive(&peers_lock);
    }
    if (pid_learned && run_mgid)
        save_mates();
}

uint32_t peer_pid(uint64_t id)
{
    uint32_t pid = 0;
    AcquireSRWLockShared(&peers_lock);
    for (int i = 0; i < MAX_PEERS; i++)
        if (peers[i].id == id)
            pid = peers[i].pid;
    ReleaseSRWLockShared(&peers_lock);
    return pid;
}

static uint32_t median_rtt(const struct peer *p)
{
    uint32_t v[5], n = p->rtt_n < 5 ? p->rtt_n : 5;
    memcpy(v, p->rtt, sizeof v);
    for (uint32_t i = 1; i < n; i++)
        for (uint32_t j = i; j && v[j - 1] > v[j]; j--) {
            uint32_t t = v[j];
            v[j] = v[j - 1], v[j - 1] = t;
        }
    return v[n / 2];
}

// Caller holds peers_lock exclusive. extra stays empty when there is nothing to add to the name.
static void build_text(struct peer *p, uint64_t now)
{
    int n = 0;
    p->extra[0] = 0;
    if (show_ping && p->rtt_n && now - p->pong_ms < PONG_TTL_MS)
        n += swprintf(p->extra + n, 32 - n, L"  %ums", median_rtt(p));
    if (show_loss && p->loss_tenths >= 10)
        swprintf(p->extra + n, 32 - n, L"  %u%%", (p->loss_tenths + 5) / 10);
}

static void tick(uint64_t now, uint64_t *loss_ms)
{
    uint64_t due[MAX_PEERS] = {0}, need_name[MAX_PEERS] = {0};
    bool window = now - *loss_ms >= LOSS_WINDOW_MS;
    if (window)
        *loss_ms = now;
    AcquireSRWLockExclusive(&peers_lock);
    for (int i = 0; i < MAX_PEERS; i++) {
        struct peer *p = &peers[i];
        if (!p->id || now - p->seen_ms > PEER_TTL_MS)
            continue;
        if (now >= p->next_ping_ms) {
            due[i] = p->id;
            p->next_ping_ms = now + (now - p->pong_ms < PONG_TTL_MS ? PING_FAST_MS : PING_PROBE_MS);
        }
        uint16_t expected = (uint16_t)(p->seq_next - p->seq_start);
        // Under 20 frames in a window says too little; keep counting into the next one.
        if (window && p->seq_on && expected >= 20) {
            uint32_t got = p->got < expected ? p->got : expected;
            p->loss_tenths = (expected - got) * 1000 / expected;
            p->seq_start = p->seq_next, p->got = 0;
        }
        if (now - p->name_ms >= NAME_REFRESH_MS) {
            need_name[i] = p->id, p->name_ms = now;
            note(LOG_DEBUG, "Ping peer %llx rtt_n=%u pong_age=%llu got=%u loss=%u", (unsigned long long)p->id,
                 p->rtt_n, (unsigned long long)(now - p->pong_ms), p->got, p->loss_tenths);
        }
        // The labels redraw at different times; a slower text keeps them showing the same value.
        if (now - p->text_ms >= TEXT_REFRESH_MS || (p->extra[0] && now - p->pong_ms >= PONG_TTL_MS))
            build_text(p, now), p->text_ms = now;
    }
    ReleaseSRWLockExclusive(&peers_lock);
    for (int i = 0; i < MAX_PEERS; i++) {
        if (due[i])
            send_msg(due[i], 1, peers[i].ping_seq++, now);
        if (need_name[i] && !friends_self)
            note(LOG_DEBUG, "Ping name %llx: no SteamFriends", (unsigned long long)need_name[i]);
        if (!need_name[i] || !friends_self)
            continue;
        wchar_t name[64] = L"";
        const char *utf8 = ((PersonaName_t)friends_orig[7])(friends_self, need_name[i]);
        if (utf8 && !MultiByteToWideChar(CP_UTF8, 0, utf8, -1, name, 64))
            name[0] = 0;
        note(LOG_DEBUG, "Ping name %llx self=%p \"%s\"", (unsigned long long)need_name[i], friends_self,
             utf8 ? utf8 : "(null)");
        AcquireSRWLockExclusive(&peers_lock);
        if (peers[i].id == need_name[i])
            memcpy(peers[i].name, name, sizeof name);
        ReleaseSRWLockExclusive(&peers_lock);
    }
}

// The game's player name labels, each passing the name in rdx: the lobby, and two shared by the HUD party list
// and the labels above teammates.
static const uint32_t sites[] = {0x9952e4, 0x7956c4, 0x79576b};

static void hook_set_ui_text(void *h, const wchar_t *s)
{
    uint32_t site = (uint32_t)((uint8_t *)__builtin_return_address(0) - 5 - (uint8_t *)GetModuleHandleA(NULL));
    int k = 0;
    while (k < 3 && sites[k] != site)
        k++;
    // At 0x7956c4 the caller two frames up tells the labels above teammates (0x998a02) from the HUD.
    bool on = hud_on;
    if (k == 0) {
        on = over_on;
    } else if (k == 1) {
        void *bt[3] = {0};
        RtlCaptureStackBackTrace(1, 3, bt, NULL);
        on = (uint8_t *)bt[2] - (uint8_t *)GetModuleHandleA(NULL) == 0x998a02 ? over_on : hud_on;
    }
    if (on && s && s[0]) {
        AcquireSRWLockShared(&peers_lock);
        for (int i = 0; i < MAX_PEERS; i++)
            if (peers[i].extra[0] && !wcscmp(s, peers[i].game_name[0] ? peers[i].game_name : peers[i].name)) {
                wchar_t text[160];
                swprintf(text, 160, L"%.120ls%ls", s, peers[i].extra);
                real_set_ui_text(h, text);
                ReleaseSRWLockShared(&peers_lock);
                return;
            }
        ReleaseSRWLockShared(&peers_lock);
    }
    if (log_level >= LOG_DEBUG && s && s[0]) {
        char line[200];
        WideCharToMultiByte(CP_UTF8, 0, s, -1, line, sizeof line, NULL, NULL);
        log_dedup("UiName", line);
    }
    // The party ready count ("N / M") goes through 0x79576b; the shared Roundtable watches it for a ready press.
    int ready = 0, n = 0;
    while (k == 2 && s && s[n] >= L'0' && s[n] <= L'9')
        ready = ready * 10 + (s[n++] - L'0');
    if (n && !wcsncmp(s + n, L" / ", 3) && s[n + 3] >= L'1' && s[n + 3] <= L'9')
        hub_ready_text(ready);
    real_set_ui_text(h, s);
}

static bool calls_set_ui_text(uint8_t *base, uint32_t rva)
{
    int32_t rel;
    if (!readable(base + rva, 5))
        return false;
    memcpy(&rel, base + rva + 1, 4);
    return base[rva] == 0xe8 && base + rva + 5 + rel == base + SET_UI_TEXT_RVA;
}

DWORD WINAPI ping_thread(LPVOID unused)
{
    (void)unused;
    show_ping = cfg("ping", "show_ping", 1), show_loss = cfg("ping", "show_loss", 1);
    hud_on = cfg("ping", "hud_labels", 1), over_on = cfg("ping", "overhead_labels", 0);
    bool hub = cfg("hub", "shared_roundtable", 0);
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpc_per_ms = f.QuadPart / 1000.0;
    real_set_ui_text = (SetUiText_t)(base + SET_UI_TEXT_RVA);
    // The code is only readable as plain x86 once the game has unpacked it. Pings are answered either way.
    for (int i = 0; (show_ping || show_loss || hub) && i < 1200; i++, Sleep(250))
        if (calls_set_ui_text(base, sites[0]) && calls_set_ui_text(base, sites[1]) && calls_set_ui_text(base, sites[2])) {
            int hooked = 0;
            for (int s = 0; s < 3; s++)
                hooked += patch_call(sites[s], SET_UI_TEXT_RVA, hook_set_ui_text, "ping name label");
            note(hooked == 3 ? LOG_INFO : LOG_WARN, "Ping labels hooked=%d of 3 ping=%d loss=%d", hooked, show_ping,
                 show_loss);
            break;
        }
    // Read every 2 ms: a slow poll on either side adds its wait to the round trip.
    for (uint64_t loss_ms = now_ms(), tick_ms = 0;; Sleep(2)) {
        if (!net_self)
            continue;
        uint64_t now = now_ms();
        read_pings(now);
        if (now - tick_ms >= 100)
            tick(now, &loss_ms), tick_ms = now;
    }
}
