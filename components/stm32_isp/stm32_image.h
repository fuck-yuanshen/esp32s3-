#pragma once
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t address;
    uint32_t length;
    uint8_t *data;          /* NULL when the payload is streamed from image_t.file */
    uint32_t file_offset;   /* absolute offset of this segment inside that file */
} image_segment_t;

typedef struct {
    image_segment_t *segments;
    size_t count;
    uint32_t low_address;
    uint32_t high_address;  /* exclusive */
    FILE *file;             /* held open for streamed raw images, NULL otherwise */
} image_t;

/* Firmware parsing is shared by both programmer backends. BIN streams from
 * the held file; HEX keeps validated addressed segments in RAM.
 */
void image_free(image_t *image);
esp_err_t image_load(const char *path, image_t *image, uint32_t base_address);
esp_err_t image_load_hex(FILE *file, image_t *image);
esp_err_t image_load_raw(FILE *file, image_t *image, uint32_t base_address);
esp_err_t image_add_segment(image_t *image, uint32_t address, const uint8_t *data, uint32_t length);
/* Merge every segment into a single aligned block; holes are erased bytes.
 * This makes sparse/unaligned HEX records program each flash halfword once.
 */
esp_err_t image_read_block(const image_t *image, uint32_t address,
                           uint8_t *data, uint32_t size, bool *present);
