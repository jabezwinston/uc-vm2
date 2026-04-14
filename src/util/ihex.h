/*
 * ucvm - Intel HEX file loader
 */
#ifndef IHEX_H
#define IHEX_H

#include <stdint.h>

/* Load an Intel HEX file into a flash buffer (word-addressed).
 * flash: output buffer of 16-bit words
 * flash_words: maximum number of words in flash buffer
 * filename: path to .hex file
 * Returns 0 on success, -1 on error (prints message to stderr). */
int ihex_load(const char *filename, uint16_t *flash, uint32_t flash_words);

/* Load from an open FILE stream */
struct _IO_FILE; /* forward decl for FILE */
int ihex_load_fp(void *fp, uint16_t *flash, uint32_t flash_words);

/* Load an Intel HEX file into a byte buffer (byte-addressed, for 8051 code).
 * buf: output buffer
 * buf_size: max bytes
 * Returns 0 on success, -1 on error. */
int ihex_load_bytes(const char *filename, uint8_t *buf, uint32_t buf_size);

#endif /* IHEX_H */
