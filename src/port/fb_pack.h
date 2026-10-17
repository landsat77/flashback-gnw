#ifndef FB_PACK_H
#define FB_PACK_H

#include <stdint.h>
#include <stdbool.h>

#define FB_PACK_MAGIC "GWFB01\0\0"

enum {
    FB_PACK_OK = 0,
    FB_PACK_ERR_MAGIC = -1,
    FB_PACK_ERR_CORRUPT = -2,
};

int fb_pack_open(const uint8_t *data, uint32_t size);
bool fb_pack_find(const char *name, const uint8_t **data, uint32_t *size);
uint32_t fb_pack_count(void);
const char *fb_pack_name(uint32_t index);

#endif
