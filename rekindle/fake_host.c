// Fake host: answers the game's P2P link to fake_host on ISteamNetworking006 channel 30.
// Raw handshake, AES-CTR frames, reliable hello and acks, then plays the answers built from the run's snapshot.
#include "rekindle.h"
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>

#define EMU_CHANNEL 30
#define EMU_SEQ0 0xc594
CRITICAL_SECTION emu_lock;
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

static bool emu_built;
uint64_t emu_mate2;

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
void *cb143, *cb506;
static volatile LONG auth_pending;
static void **user_orig;
typedef int (*BeginAuth_t)(void *, const void *, int, uint64_t);
typedef void (*EndAuth_t)(void *, uint64_t);

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

// Runs after the game's RunCallbacks: the fake auth answer, then the fake host leaving once the stream is done.
void emu_callbacks(void)
{
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

// Takes ownership of the answer buffers answers.c built from the snapshot.
void emu_set_answers(uint8_t *intro, size_t intro_n, uint8_t *ans, size_t ans_n, uint8_t *str, size_t str_n)
{
    EnterCriticalSection(&emu_lock);
    emu_replay = intro, emu_replay_len = intro_n;
    emu_answers = ans, emu_answers_len = ans_n;
    emu_stream = str, emu_stream_len = str_n;
    emu_built = true;
    LeaveCriticalSection(&emu_lock);
}

void emu_wrap_user(void **vt, void **nv)
{
    user_orig = vt;
    nv[14] = us_begin;
    nv[15] = us_end;
}

void emu_wrap_net(void **vt, void **nv)
{
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
