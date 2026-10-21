/*
 * Disk images as GCR tracks (see gcr.c).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GCR_HALF_TRACKS     84          /* per side: tracks 1-42 and the halves between */

typedef struct gcr_disk {
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
/* A sector's 256 bytes as the head reads them from the GCR now (tracks
 * 1-70: a D71's side 2 is 36-70; a G64's 1-42); false if its header or
 * data block isn't found or is bad
 */
bool gcr_read_sector(const gcr_disk_t *d, int track, int sector, uint8_t *out);
int gcr_sectors(int track);
int gcr_zone(int track);
