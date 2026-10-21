/*
 * Disk images as GCR: what the drive's head reads.
 *
 * .D64 (35 or 40 tracks, with or without the error bytes) and .D71 (two
 * sides of 35 tracks) are encoded into GCR tracks as a 1541 formats them: per sector a sync, the header block, a gap, a
 * sync, the data block and a gap, the gaps sized to fill the track at its
 * zone's speed. The error bytes become the errors the DOS sees (missing
 * header or data block, bad checksums, no sync, wrong ID). .G64 holds the
 * GCR itself, half tracks and protections included.
 *
 * Writes are kept in the GCR; flushing decodes the changed tracks back to
 * sectors (D64, D71), or writes the GCR as it is (G64, track lengths
 * unchanged).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cbm_internal.h"
#include "gcr.h"

static const uint8_t gcr_code[16] = {
    0x0A, 0x0B, 0x12, 0x13, 0x0E, 0x0F, 0x16, 0x17, 0x09, 0x19, 0x1A, 0x1B, 0x0D, 0x1D, 0x1E, 0x15,
};
static int8_t gcr_decode_table[32];

int gcr_sectors(int track, int sides)
{
    if (sides == 2) {
        track = (track - 1) % 35 + 1;   /* side 2 of a D71 counts again */
    }
    return track <= 17 ? 21 : track <= 24 ? 19 : track <= 30 ? 18 : 17;
}

int gcr_zone(int track, int sides)
{
    if (sides == 2) {
        track = (track - 1) % 35 + 1;
    }
    return track <= 17 ? 3 : track <= 24 ? 2 : track <= 30 ? 1 : 0;
}

/* Where track t lies in gcr_disk_t.track[]: a D71's 36-70 on its second side */
static int track_index(int track, int sides)
{
    return sides == 2 && track > 35 ? GCR_HALF_TRACKS + (track - 36) * 2 : (track - 1) * 2;
}

static const uint16_t zone_bytes[4] = { 6250, 6666, 7142, 7692 };

/* 4 bytes -> 5 GCR bytes */
static void encode4(const uint8_t *in, uint8_t *out)
{
    uint64_t bits = 0;
    for (int i = 0; i < 4; i++) {
        bits = (bits << 10) | (gcr_code[in[i] >> 4] << 5) | gcr_code[in[i] & 15];
    }
    for (int i = 4; i >= 0; i--) {
        out[i] = bits & 0xFF;
        bits >>= 8;
    }
}

/* 5 GCR bytes -> 4 bytes; false if a code isn't valid */
static bool decode5(const uint8_t *in, uint8_t *out)
{
    uint64_t bits = 0;
    for (int i = 0; i < 5; i++) {
        bits = (bits << 8) | in[i];
    }
    bool ok = true;
    for (int i = 3; i >= 0; i--) {
        int lo = gcr_decode_table[bits & 31], hi = gcr_decode_table[(bits >> 5) & 31];
        bits >>= 10;
        if (lo < 0 || hi < 0) {
            ok = false;
        }
        out[i] = ((hi & 15) << 4) | (lo & 15);
    }
    return ok;
}

static void init_tables(void)
{
    memset(gcr_decode_table, -1, sizeof(gcr_decode_table));
    for (int i = 0; i < 16; i++) {
        gcr_decode_table[gcr_code[i]] = i;
    }
}

/* ---- Encoding a D64 track ---- */

static uint32_t sector_offset(int track, int sector, int sides)
{
    uint32_t off = 0;
    for (int t = 1; t < track; t++) {
        off += gcr_sectors(t, sides);
    }
    return (off + sector) * 256;
}

/* Track `track` (1-70) of an image into GCR; errors: per sector or NULL */
static uint8_t *encode_track(const uint8_t *image, int track, int sides, const uint8_t *errors, const uint8_t id[2],
                             uint16_t *len)
{
    int zone = gcr_zone(track, sides), n = gcr_sectors(track, sides);
    uint16_t total = zone_bytes[zone];
    uint8_t *t = cbm_malloc(total);
    if (t == NULL) {
        return NULL;
    }
    memset(t, 0x55, total);
    int per_sector = 5 + 10 + 9 + 5 + 325;
    int gap = (total - n * per_sector) / n;
    uint8_t *p = t;
    for (int s = 0; s < n; s++) {
        const uint8_t *data = image + sector_offset(track, s, sides);
        uint8_t err = errors ? errors[sector_offset(track, s, sides) / 256] : 1;
        uint8_t id1 = id[0], id2 = id[1];
        if (err == 0x0B || err == 29) {
            id1 ^= 0xFF;                /* disk ID mismatch */
        }
        /* header */
        /* (side 2 of a D71: tracks 36-70 in the headers, as a 1571 writes them) */
        uint8_t h[8] = { 0x08, 0, (uint8_t)s, (uint8_t)track, id2, id1, 0x0F, 0x0F };
        h[1] = h[2] ^ h[3] ^ h[4] ^ h[5];
        if (err == 0x09 || err == 27) {
            h[1] ^= 0xFF;
        }
        if (err == 0x02 || err == 20) {
            h[0] = 0x00;                /* header block not found */
        }
        bool sync = !(err == 0x03 || err == 21);
        memset(p, sync ? 0xFF : 0x55, 5);
        p += 5;
        encode4(h, p);
        encode4(h + 4, p + 5);
        p += 10;
        p += 9;                         /* gap (0x55) */
        memset(p, sync ? 0xFF : 0x55, 5);
        p += 5;
        uint8_t block[260];
        block[0] = (err == 0x04 || err == 22) ? 0x00 : 0x07;
        memcpy(block + 1, data, 256);
        uint8_t ck = 0;
        for (int i = 0; i < 256; i++) {
            ck ^= data[i];
        }
        block[257] = (err == 0x05 || err == 23) ? ck ^ 0xFF : ck;
        block[258] = block[259] = 0;
        for (int i = 0; i < 260; i += 4) {
            encode4(block + i, p);
            p += 5;
        }
        p += gap;
    }
    *len = total;
    return t;
}

/* ---- Decoding a track back into its sectors (writes) ---- */

static void decode_track(const uint8_t *t, uint16_t len, int track, int sides, uint8_t *image, uint32_t *written)
{
    int n = gcr_sectors(track, sides);
    int sector = -1;
    for (uint32_t i = 0; i < (uint32_t)len * 2; i++) {
        /* a sync: two 0xFF, then the block after the last of them */
        if (t[i % len] != 0xFF || t[(i + 1) % len] != 0xFF) {
            continue;
        }
        uint32_t j = i;
        while (t[j % len] == 0xFF && j < i + len) {
            j++;
        }
        uint8_t gcr[325], b[260];
        for (int k = 0; k < 325; k++) {
            gcr[k] = t[(j + k) % len];
        }
        decode5(gcr, b);
        if (b[0] == 0x08) {
            decode5(gcr + 5, b + 4);
            sector = b[2] < n && b[3] == track ? b[2] : -1;
        } else if (b[0] == 0x07 && sector >= 0) {
            for (int k = 1; k < 65; k++) {
                decode5(gcr + k * 5, b + k * 4);
            }
            uint8_t *dst = image + sector_offset(track, sector, sides);
            if (memcmp(dst, b + 1, 256) != 0) {
                memcpy(dst, b + 1, 256);
                (*written)++;
            }
            sector = -1;
        }
        i = j + 9;
        if (i >= len) {
            break;
        }
    }
}

bool gcr_read_sector(const gcr_disk_t *d, int track, int sector, uint8_t *out)
{
    int i = d->g64 ? (track - 1) * 2 : track_index(track, d->sides);
    if (track < 1 || i >= GCR_HALF_TRACKS * 2 || d->track[i] == NULL || d->len[i] == 0) {
        return false;
    }
    const uint8_t *t = d->track[i];
    uint32_t len = d->len[i];
    bool header = false;
    /* (as decode_track(), over two turns: a sector may wrap round) */
    for (uint32_t p = 0; p < len * 2; p++) {
        if (t[p % len] != 0xFF || t[(p + 1) % len] != 0xFF) {
            continue;
        }
        uint32_t j = p;
        while (t[j % len] == 0xFF && j < p + len) {
            j++;
        }
        uint8_t gcr[325], b[260];
        for (int k = 0; k < 325; k++) {
            gcr[k] = t[(j + k) % len];
        }
        decode5(gcr, b);
        if (b[0] == 0x08) {
            header = b[2] == sector && b[3] == track;
        } else if (b[0] == 0x07 && header) {
            bool ok = true;
            uint8_t ck = 0;
            for (int k = 1; k < 65; k++) {
                ok = decode5(gcr + k * 5, b + k * 4) && ok;
            }
            for (int k = 1; k <= 256; k++) {
                ck ^= b[k];
            }
            if (!ok || ck != b[257]) {
                return false;
            }
            memcpy(out, b + 1, 256);
            return true;
        }
        p = j + 9;
    }
    return false;
}

/* ---- Images ---- */

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    if (n < e) {
        return false;
    }
    for (size_t i = 0; i < e; i++) {
        char c = name[n - e + i];
        c = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        if (c != ext[i]) {
            return false;
        }
    }
    return true;
}

void gcr_free(gcr_disk_t *d)
{
    for (int i = 0; i < GCR_HALF_TRACKS * 2; i++) {
        free(d->track[i]);
    }
    free(d->image);
    memset(d, 0, sizeof(*d));
}

static bool load_g64(gcr_disk_t *d, const uint8_t *f, long size)
{
    if (size < 12 || memcmp(f, "GCR-1541", 8) != 0) {
        return false;
    }
    int tracks = f[9];
    if (tracks > GCR_HALF_TRACKS) {
        tracks = GCR_HALF_TRACKS;
    }
    for (int i = 0; i < tracks; i++) {
        uint32_t off = f[12 + i * 4] | (f[13 + i * 4] << 8) | (f[14 + i * 4] << 16) | ((uint32_t)f[15 + i * 4] << 24);
        if (off == 0 || off + 2 > (uint32_t)size) {
            continue;
        }
        uint16_t len = f[off] | (f[off + 1] << 8);
        if (len == 0 || off + 2 + len > (uint32_t)size) {
            continue;
        }
        d->track[i] = cbm_malloc(len);
        if (d->track[i] == NULL) {
            return false;
        }
        memcpy(d->track[i], f + off + 2, len);
        d->len[i] = len;
        d->file_off[i] = off + 2;
    }
    d->g64 = true;
    return true;
}

static bool load_d64(gcr_disk_t *d, const uint8_t *f, long size, bool d71)
{
    int tracks, sides = d71 ? 2 : 1;
    bool errors = false;
    if (d71) {
        if (size != 349696 && size != 351062) {
            return false;
        }
        tracks = 70;
        errors = size == 351062;
    } else {
        switch (size) {
        case 174848: tracks = 35; break;
        case 175531: tracks = 35; errors = true; break;
        case 196608: tracks = 40; break;
        case 197376: tracks = 40; errors = true; break;
        default: return false;
        }
    }
    uint32_t blocks = 0;
    for (int t = 1; t <= tracks; t++) {
        blocks += gcr_sectors(t, sides);
    }
    d->image = cbm_malloc(blocks * 256);
    if (d->image == NULL) {
        return false;
    }
    memcpy(d->image, f, blocks * 256);
    d->image_size = blocks * 256;
    d->tracks = tracks;
    const uint8_t *err = errors ? f + blocks * 256 : NULL;
    uint8_t id[2];
    const uint8_t *bam = d->image + sector_offset(18, 0, sides);
    id[0] = bam[0xA2];
    id[1] = bam[0xA3];
    for (int t = 1; t <= tracks; t++) {
        int i = track_index(t, sides);
        d->track[i] = encode_track(d->image, t, sides, err, id, &d->len[i]);
        if (d->track[i] == NULL) {
            return false;
        }
    }
    d->sides = sides;
    return true;
}

bool gcr_load(gcr_disk_t *d, const char *path)
{
    static bool tables;
    if (!tables) {
        init_tables();
        tables = true;
    }
    memset(d, 0, sizeof(*d));
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *f = size > 0 && size <= 1024 * 1024 ? cbm_malloc(size) : NULL;
    bool ok = f && fread(f, 1, size, fp) == (size_t)size;
    fclose(fp);
    if (ok) {
        if (has_ext(path, ".g64")) {
            ok = load_g64(d, f, size);
            d->sides = 1;
        } else {
            ok = load_d64(d, f, size, has_ext(path, ".d71"));
        }
    }
    free(f);
    if (!ok) {
        gcr_free(d);
        return false;
    }
    snprintf(d->path, sizeof(d->path), "%s", path);
    FILE *rw = fopen(path, "r+b");
    d->wp = rw == NULL;
    if (rw) {
        fclose(rw);
    }
    return true;
}

bool gcr_dirty(const gcr_disk_t *d)
{
    for (int i = 0; i < GCR_HALF_TRACKS * 2; i++) {
        if (d->dirty[i]) {
            return true;
        }
    }
    return false;
}

bool gcr_flush(gcr_disk_t *d, uint32_t *sectors_written)
{
    if (d->wp || !gcr_dirty(d)) {
        return true;
    }
    FILE *fp = fopen(d->path, "r+b");
    if (fp == NULL) {
        return false;
    }
    bool ok = true;
    for (int i = 0; i < GCR_HALF_TRACKS * 2 && ok; i++) {
        if (!d->dirty[i] || d->track[i] == NULL) {
            continue;
        }
        d->dirty[i] = false;
        if (d->g64) {
            ok = fseek(fp, d->file_off[i], SEEK_SET) == 0 && fwrite(d->track[i], 1, d->len[i], fp) == d->len[i];
            continue;
        }
        int side = i >= GCR_HALF_TRACKS, half = i % GCR_HALF_TRACKS;
        if (half & 1) {
            continue;
        }
        int track = half / 2 + 1 + side * 35;
        if (track > d->tracks) {
            continue;
        }
        uint32_t written = 0;
        decode_track(d->track[i], d->len[i], track, d->sides, d->image, &written);
        if (written) {
            uint32_t off = sector_offset(track, 0, d->sides), n = gcr_sectors(track, d->sides) * 256;
            ok = fseek(fp, off, SEEK_SET) == 0 && fwrite(d->image + off, 1, n, fp) == n;
            if (sectors_written) {
                *sectors_written += written;
            }
        }
    }
    fclose(fp);
    return ok;
}

bool gcr_create(const char *path, bool d71, const char *name)
{
    uint32_t size = d71 ? 349696 : 174848;
    int sides = d71 ? 2 : 1;
    uint8_t *img = cbm_calloc(1, size);
    if (img == NULL) {
        return false;
    }
    uint8_t *bam = img + sector_offset(18, 0, sides);
    bam[0] = 18;
    bam[1] = 1;
    bam[2] = 0x41;                      /* 'A': 1541 format */
    bam[3] = d71 ? 0x80 : 0x00;         /* double-sided flag */
    for (int t = 1; t <= 35; t++) {
        int n = gcr_sectors(t, sides);
        uint8_t *e = bam + 4 * t;
        uint32_t bits = (1u << n) - 1;
        if (t == 18) {
            bits &= ~3u;                /* BAM and the first directory sector */
            n -= 2;
        }
        e[0] = n;
        e[1] = bits;
        e[2] = bits >> 8;
        e[3] = bits >> 16;
    }
    memset(bam + 0x90, 0xA0, 0x1B);
    memcpy(bam + 0x90, name, strlen(name) < 16 ? strlen(name) : 16);
    bam[0xA2] = '0';
    bam[0xA3] = '1';
    bam[0xA5] = '2';
    bam[0xA6] = 'A';
    if (d71) {
        /* side 2's free counts at $DD, its bitmap in track 53 sector 0 */
        uint8_t *bam2 = img + sector_offset(53, 0, sides);
        for (int t = 36; t <= 70; t++) {
            int n = gcr_sectors(t, sides);
            uint32_t bits = (1u << n) - 1;
            if (t == 53) {
                bits = 0;               /* reserved on a 1571 */
                n = 0;
            }
            bam[0xDD + t - 36] = n;
            uint8_t *e = bam2 + (t - 36) * 3;
            e[0] = bits;
            e[1] = bits >> 8;
            e[2] = bits >> 16;
        }
    }
    uint8_t *dir = img + sector_offset(18, 1, sides);
    dir[1] = 0xFF;
    FILE *fp = fopen(path, "wb");
    bool ok = fp && fwrite(img, 1, size, fp) == size;
    if (fp) {
        fclose(fp);
    }
    free(img);
    return ok;
}
