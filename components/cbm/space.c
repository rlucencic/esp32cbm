/*
 * Free space on the disks in drives 8 and 9, for the web page: the BAM's
 * "blocks free" (esp32emu's diskspace), its sector read from the GCR as
 * the drive's head would find it (D64, D71 and G64 alike, unflushed
 * writes too).
 *
 * Worked out at insert and after writes are flushed (both with the
 * emulation stopped), kept for the web page's task.
 */
#include <string.h>
#include "cbm_internal.h"
#include "diskspace.h"
#include "gcr.h"

typedef struct {
    volatile bool known;
    volatile uint64_t free, size;
} space_t;

static space_t s_space[CBM_DRIVES];

/* Bytes in D64 order: track 1 sector 0 first (side 2 of a D71 from track 36) */
static bool gcr_read(void *ctx, uint64_t offset, void *buf, uint32_t len)
{
    const gcr_disk_t *d = ctx;
    uint8_t *out = buf, sector[256];
    while (len > 0) {
        uint32_t block = (uint32_t)(offset / 256), at = (uint32_t)(offset % 256), n = 256 - at;
        int track = 1, last = d->sides == 2 ? 70 : 42;
        while (track <= last && block >= (uint32_t)gcr_sectors(track, d->sides)) {
            block -= gcr_sectors(track, d->sides);
            track++;
        }
        if (track > last || !gcr_read_sector(d, track, (int)block, sector)) {
            return false;
        }
        n = n < len ? n : len;
        memcpy(out, sector + at, n);
        out += n;
        offset += n;
        len -= n;
    }
    return true;
}

void space_update(int drive, const gcr_disk_t *disk)
{
    if (drive < 0 || drive >= CBM_DRIVES) {
        return;
    }
    diskspace_t s = { 0 };
    bool known = false;
    if (disk != NULL) {
        /* the size the counter knows the format by */
        bool forty = disk->g64 ? disk->track[70] != NULL : disk->tracks == 40;
        uint64_t size = disk->sides == 2 ? 349696 : forty ? 196608 : 174848;
        diskspace_disk_t d = { gcr_read, (void *)disk, size };
        known = diskspace_cbm(&d, &s);
    }
    s_space[drive].known = false;
    s_space[drive].free = s.free;
    s_space[drive].size = s.size;
    s_space[drive].known = known;
}

bool cbm_disk_space(int drive, uint64_t *free_bytes, uint64_t *size)
{
    if (drive < 0 || drive >= CBM_DRIVES || !s_space[drive].known) {
        return false;
    }
    *free_bytes = s_space[drive].free;
    *size = s_space[drive].size;
    return s_space[drive].known && *size > 0;
}
