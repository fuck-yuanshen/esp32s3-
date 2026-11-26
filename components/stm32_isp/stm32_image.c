#include "stm32_image.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
static const char *TAG = "stm32_image";

void image_free(image_t *image)
{
    if (!image) {
        return;
    }
    for (size_t i = 0; i < image->count; ++i) {
        free(image->segments[i].data);
    }
    free(image->segments);
    if (image->file) {
        fclose(image->file);
    }
    memset(image, 0, sizeof(*image));
}

esp_err_t image_add_segment(image_t *image, uint32_t address,
                                   const uint8_t *data, uint32_t length)
{
    if (!length) {
        return ESP_OK;
    }
    if (address > UINT32_MAX - length) return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < image->count; ++i) {
        const image_segment_t *seg = &image->segments[i];
        uint32_t lo = address > seg->address ? address : seg->address;
        uint32_t hi = address + length < seg->address + seg->length
                        ? address + length : seg->address + seg->length;
        if (lo < hi && memcmp(data + lo - address,
                              seg->data + lo - seg->address, hi - lo) != 0)
            return ESP_ERR_INVALID_RESPONSE;
    }
    /* Intel HEX arrives as one record per line (16 bytes by default). Appending
     * a contiguous record to the previous segment keeps the write loop at one
     * 256-byte block per pass instead of re-sending the same block per record.
     * A non-contiguous record starts a new segment instead.
     */
    if (image->count) {
        image_segment_t *last = &image->segments[image->count - 1];
        const uint32_t last_end = last->address + last->length;
        if (address >= last->address && address <= last_end) {
            const uint32_t end_address = (address + length > last_end) ? address + length : last_end;
            uint8_t *grown_data = realloc(last->data, end_address - last->address);
            if (!grown_data) {
                return ESP_ERR_NO_MEM;
            }
            memcpy(&grown_data[address - last->address], data, length);
            last->data = grown_data;
            last->length = end_address - last->address;
            if (last->address + last->length > image->high_address) {
                image->high_address = last->address + last->length;
            }
            return ESP_OK;
        }
    }

    image_segment_t *grown = realloc(image->segments,
                                     (image->count + 1) * sizeof(image_segment_t));
    if (!grown) {
        return ESP_ERR_NO_MEM;
    }
    image->segments = grown;
    image_segment_t *segment = &image->segments[image->count];
    segment->data = malloc(length);
    if (!segment->data) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(segment->data, data, length);
    segment->address = address;
    segment->length = length;
    segment->file_offset = 0;
    image->count++;

    if (!image->low_address || address < image->low_address) {
        image->low_address = address;
    }
    if (address + length > image->high_address) {
        image->high_address = address + length;
    }
    return ESP_OK;
}

static int intel_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Decode and validate Intel HEX data, EOF, and address records. Start-address
 * records are validated but do not affect STM32 bootloader programming.
 */
esp_err_t image_load_hex(FILE *file, image_t *image)
{
    char line[600];
    uint32_t upper = 0;
    bool eof_seen = false;
    while (fgets(line, sizeof(line), file)) {
        char *text = line;
        if (strlen(text) >= 3 && (uint8_t)text[0] == 0xEF &&
            (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) {
            text += 3;
        }
        while (*text == ' ' || *text == '\t') text++;
        size_t text_length = strlen(text);
        while (text_length && (text[text_length - 1] == '\r' ||
                               text[text_length - 1] == '\n' ||
                               text[text_length - 1] == ' ' ||
                               text[text_length - 1] == '\t')) {
            text[--text_length] = '\0';
        }
        if (!text_length) continue;
        if (eof_seen || text[0] != ':') {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (text_length < 11 || ((text_length - 1) & 1U)) {
            return ESP_ERR_INVALID_SIZE;
        }

        uint8_t record[260];
        const size_t byte_count = (text_length - 1) / 2;
        if (byte_count > sizeof(record)) {
            return ESP_ERR_INVALID_SIZE;
        }
        for (size_t i = 0; i < byte_count; ++i) {
            const int high = intel_hex_nibble(text[1 + i * 2]);
            const int low = intel_hex_nibble(text[2 + i * 2]);
            if (high < 0 || low < 0) return ESP_ERR_INVALID_RESPONSE;
            record[i] = (uint8_t)((high << 4) | low);
        }
        const uint8_t length = record[0];
        if (byte_count != (size_t)length + 5) {
            return ESP_ERR_INVALID_SIZE;
        }
        uint8_t checksum = 0;
        for (size_t i = 0; i < byte_count; ++i) checksum += record[i];
        if (checksum != 0) return ESP_ERR_INVALID_CRC;

        const uint16_t offset = (uint16_t)((record[1] << 8) | record[2]);
        const uint8_t type = record[3];
        switch (type) {
        case 0x00: {
            if (upper > UINT32_MAX - offset ||
                upper + offset > UINT32_MAX - length) {
                return ESP_ERR_INVALID_SIZE;
            }
            esp_err_t ret = image_add_segment(image, upper + offset, &record[4], length);
            if (ret != ESP_OK) return ret;
            break;
        }
        case 0x01:
            if (length != 0 || offset != 0) return ESP_ERR_INVALID_RESPONSE;
            eof_seen = true;
            break;
        case 0x02:
            if (length != 2 || offset != 0) return ESP_ERR_INVALID_RESPONSE;
            upper = ((uint32_t)((record[4] << 8) | record[5])) << 4;
            break;
        case 0x03:
            if (length != 4 || offset != 0) return ESP_ERR_INVALID_RESPONSE;
            break;
        case 0x04:
            if (length != 2 || offset != 0) return ESP_ERR_INVALID_RESPONSE;
            upper = ((uint32_t)((record[4] << 8) | record[5])) << 16;
            break;
        case 0x05:
            if (length != 4 || offset != 0) return ESP_ERR_INVALID_RESPONSE;
            break;
        default:
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    if (!eof_seen) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return image->count ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

/* Raw .bin images stay on SPIFFS and are streamed 256 bytes at a time. Staging
 * the whole file in RAM fails once an image passes a few hundred kilobytes on
 * an ESP32-S3, and the write loop never needs more than one block.
 */
esp_err_t image_load_raw(FILE *file, image_t *image, uint32_t base_address)
{
    if (fseek(file, 0, SEEK_END) != 0) {
        return ESP_FAIL;
    }
    const long size = ftell(file);
    if (size <= 0 || (uint64_t)size > UINT32_MAX - base_address) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    image_segment_t *segment = realloc(image->segments, sizeof(image_segment_t));
    if (!segment) {
        return ESP_ERR_NO_MEM;
    }
    image->segments = segment;
    segment->address = base_address;
    segment->length = (uint32_t)size;
    segment->data = NULL;
    segment->file_offset = 0;
    image->count = 1;
    image->low_address = base_address;
    image->high_address = base_address + (uint32_t)size;
    image->file = file;
    return ESP_OK;
}

esp_err_t image_load(const char *path, image_t *image, uint32_t base_address)
{
    memset(image, 0, sizeof(*image));
    FILE *file = fopen(path, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Accept UTF-8 BOM before Intel HEX; all other images are raw binary. */
    uint8_t prefix[3] = {0};
    const size_t prefix_size = fread(prefix, 1, sizeof(prefix), file);
    const bool has_bom = prefix_size == sizeof(prefix) &&
                         prefix[0] == 0xEF && prefix[1] == 0xBB && prefix[2] == 0xBF;
    if (fseek(file, has_bom ? 3 : 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    const int first = fgetc(file);
    if (fseek(file, has_bom ? 3 : 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    esp_err_t ret = (first == ':') ? image_load_hex(file, image)
                                   : image_load_raw(file, image, base_address);
    /* A raw image keeps the handle open for streaming and closes it in
     * image_free(); parsed HEX data lives in RAM, so the handle closes here.
     */
    if (image->file != file) {
        fclose(file);
    } else if (ret != ESP_OK) {
        fclose(file);
        image->file = NULL;
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "image %s: %u segment(s), 0x%08X..0x%08X", path,
                 (unsigned)image->count, (unsigned)image->low_address,
                 (unsigned)image->high_address);
    }
    if (ret != ESP_OK) image_free(image);
    return ret;
}

esp_err_t image_read_block(const image_t *image, uint32_t address,
                           uint8_t *data, uint32_t size, bool *present)
{
    if (!data || !present || !size || address > UINT32_MAX - size)
        return ESP_ERR_INVALID_ARG;
    memset(data, 0xFF, size);
    *present = false;
    for (size_t i = 0; i < image->count; ++i) {
        const image_segment_t *s = &image->segments[i];
        uint32_t lo = address > s->address ? address : s->address;
        uint32_t hi = address + size < s->address + s->length
                        ? address + size : s->address + s->length;
        if (lo >= hi) continue;
        *present = true;
        uint8_t *dst = data + lo - address;
        if (s->data) memcpy(dst, s->data + lo - s->address, hi - lo);
        else if (!image->file ||
                 fseek(image->file, (long)(s->file_offset + lo - s->address), SEEK_SET) ||
                 fread(dst, 1, hi - lo, image->file) != hi - lo) return ESP_FAIL;
    }
    return ESP_OK;
}
