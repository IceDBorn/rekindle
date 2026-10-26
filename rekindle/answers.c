// Fake host answers built from the run's snapshot: request answers, the 0x37/0x5e stream and the intro frames.
#include "rekindle.h"
#include <stdlib.h>
#include <string.h>
#include "snapshot_format.h"

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
bool build_answers(const uint8_t *s, size_t n, uint64_t *clock, uint8_t box[20])
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
        if (ty == SNAP_POSITION && l == 20 && cfg("restore", "restore_position", 1)) {
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
    emu_set_answers(intro.p, intro.n, ans.p, ans.n, str.p, str.n);
    note(LOG_DEBUG, "Auto answers=%u stream=%u intro=%u", (unsigned)ans.n, (unsigned)str.n, (unsigned)intro.n);
    return have_box;
}
