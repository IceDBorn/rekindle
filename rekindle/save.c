// Save planting: USER_DATA010 in NR0000.sl2 gets a rejoin record for the fake lobby, so the game offers to
// resume the crashed run.
#include "rekindle.h"
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int plant_save(const char *path, uint64_t mgid, const uint8_t box[20], bool have_box)
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
uint64_t find_self(const char *appdata)
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
