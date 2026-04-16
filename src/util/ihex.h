/*
 * ucvm - Firmware file loaders
 *
 * Supports:
 *   Intel HEX (.hex, .ihx) — standard toolchain output
 *   UCVM binary (.bin)      — compact, fast to load
 *
 * Binary format:
 *   Header (8 bytes):
 *     magic[4]     = "UCFM"
 *     range_count  = uint8_t
 *     flags        = uint8_t
 *     reserved[2]
 *   Per range (8 bytes):
 *     start_addr   = uint32_t (little-endian)
 *     length       = uint32_t (little-endian)
 *   Raw data follows each range descriptor (length bytes).
 */
#ifndef IHEX_H
#define IHEX_H

#include <stdint.h>

/* ---- Intel HEX ---- */

/* Load Intel HEX into word-addressed flash (AVR). */
int ihex_load(const char *filename, uint16_t *flash, uint32_t flash_words);

/* Load from an open FILE stream. */
int ihex_load_fp(void *fp, uint16_t *flash, uint32_t flash_words);

/* Load Intel HEX into byte-addressed buffer (8051). */
int ihex_load_bytes(const char *filename, uint8_t *buf, uint32_t buf_size);

/* ---- UCVM binary ---- */

#define UCFM_MAGIC "UCFM"

typedef struct __attribute__((packed)) {
    uint8_t  magic[4];       /* "UCFM" */
    uint8_t  range_count;
    uint8_t  flags;          /* bit 0: word-addressed (AVR) */
    uint8_t  reserved[2];
} ucfm_header_t;             /* 8 bytes */

#define UCFM_FLAG_WORD_ADDR  0x01   /* data is 16-bit words, not bytes */

typedef struct __attribute__((packed)) {
    uint32_t start_addr;     /* byte address */
    uint32_t length;         /* bytes of data following this descriptor */
} ucfm_range_t;              /* 8 bytes */

/* Load UCVM binary into word-addressed flash (AVR). */
int ucfm_load(const char *filename, uint16_t *flash, uint32_t flash_words);

/* Load UCVM binary into byte-addressed buffer (8051). */
int ucfm_load_bytes(const char *filename, uint8_t *buf, uint32_t buf_size);

/* Save flash/code to UCVM binary. */
int ucfm_save(const char *filename, const void *data, uint32_t size,
              uint32_t start_addr, uint8_t flags);

/* Detect format by extension or content. Returns 'h' for hex, 'b' for bin. */
int fw_detect(const char *filename);

/* Load firmware (auto-detect format) into word-addressed flash. */
int fw_load(const char *filename, uint16_t *flash, uint32_t flash_words);

/* Load firmware (auto-detect format) into byte-addressed buffer. */
int fw_load_bytes(const char *filename, uint8_t *buf, uint32_t buf_size);

#endif /* IHEX_H */
