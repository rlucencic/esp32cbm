/*
 * Disk images as GCR tracks (see gcr.c).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GCR_HALF_TRACKS     84          /* per side: tracks 1-42 and the halves between */

typedef struct {
    char path[256];
    bool wp, g64;
    int sides, tracks;
    /* [side * GCR_HALF_TRACKS + half track]; track t is half track (t - 1) * 2 */
    uint8_t *track[GCR_HALF_TRACKS * 2];
    uint16_t len[GCR_HALF_TRACKS * 2];
    bool dirty[GCR_HALF_TRACKS * 2];
    uint32_t file_off[GCR_HALF_TRACKS * 2];     /* G64 */
    uint8_t *image;                     /* D64/D71: the sectors */
    uint32_t image_size;
} gcr_disk_t;

bool gcr_load(gcr_disk_t *d, const char *path);
void gcr_free(gcr_disk_t *d);
bool gcr_dirty(const gcr_disk_t *d);
bool gcr_flush(gcr_disk_t *d, uint32_t *sectors_written);
bool gcr_create(const char *path, bool d71, const char *name);
int gcr_sectors(int track);
int gcr_zone(int track);
