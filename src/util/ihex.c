/*
 * ucvm - Intel HEX file loader
 *
 * Parses Intel HEX format (.hex) files produced by avr-objcopy.
 * Supports record types 00 (data) and 01 (EOF).
 * Extended address records (02, 04) are handled for completeness.
 */
#include "ihex.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Parse one hex byte (2 ASCII chars) */
static int parse_hex_byte(const char *s)
{
    int hi, lo;
    if (s[0] >= '0' && s[0] <= '9') hi = s[0] - '0';
    else if (s[0] >= 'A' && s[0] <= 'F') hi = s[0] - 'A' + 10;
    else if (s[0] >= 'a' && s[0] <= 'f') hi = s[0] - 'a' + 10;
    else return -1;
    if (s[1] >= '0' && s[1] <= '9') lo = s[1] - '0';
    else if (s[1] >= 'A' && s[1] <= 'F') lo = s[1] - 'A' + 10;
    else if (s[1] >= 'a' && s[1] <= 'f') lo = s[1] - 'a' + 10;
    else return -1;
    return (hi << 4) | lo;
}

int ihex_load_fp(void *fp_void, uint16_t *flash, uint32_t flash_words)
{
    FILE *fp = fp_void;
    char line[600];
    uint32_t base_addr = 0;
    int line_num = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        /* Strip newline */
        char *p = line;
        size_t len = strlen(p);
        while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r'))
            p[--len] = '\0';

        if (len == 0)
            continue;

        /* Must start with ':' */
        if (p[0] != ':') {
            fprintf(stderr, "ihex: line %d: missing ':'\n", line_num);
            return -1;
        }
        p++;
        len--;

        if (len < 10) {
            fprintf(stderr, "ihex: line %d: too short\n", line_num);
            return -1;
        }

        /* Parse fields */
        int byte_count = parse_hex_byte(p);
        int addr_hi    = parse_hex_byte(p + 2);
        int addr_lo    = parse_hex_byte(p + 4);
        int rec_type   = parse_hex_byte(p + 6);

        if (byte_count < 0 || addr_hi < 0 || addr_lo < 0 || rec_type < 0) {
            fprintf(stderr, "ihex: line %d: parse error\n", line_num);
            return -1;
        }

        uint16_t addr = (addr_hi << 8) | addr_lo;

        /* Verify checksum */
        uint8_t checksum = 0;
        for (int i = 0; i < (int)len; i += 2) {
            int b = parse_hex_byte(p + i);
            if (b < 0) {
                fprintf(stderr, "ihex: line %d: bad hex at pos %d\n", line_num, i);
                return -1;
            }
            checksum += (uint8_t)b;
        }
        if (checksum != 0) {
            fprintf(stderr, "ihex: line %d: checksum error\n", line_num);
            return -1;
        }

        switch (rec_type) {
        case 0x00: {
            /* Data record */
            uint8_t *data_p = (uint8_t *)p + 8; /* skip byte_count, addr, type */
            for (int i = 0; i < byte_count; i++) {
                int b = parse_hex_byte((char *)data_p + i * 2);
                if (b < 0) {
                    fprintf(stderr, "ihex: line %d: data parse error\n", line_num);
                    return -1;
                }
                uint32_t byte_addr = base_addr + addr + i;
                uint32_t word_idx = byte_addr >> 1;
                if (word_idx >= flash_words)
                    continue; /* silently skip data beyond flash size */
                if (byte_addr & 1)
                    flash[word_idx] = (flash[word_idx] & 0x00FF) | ((uint16_t)b << 8);
                else
                    flash[word_idx] = (flash[word_idx] & 0xFF00) | (uint16_t)b;
            }
            break;
        }
        case 0x01:
            /* End of file */
            return 0;
        case 0x02: {
            /* Extended segment address */
            if (byte_count != 2) {
                fprintf(stderr, "ihex: line %d: bad ext seg addr\n", line_num);
                return -1;
            }
            int hi = parse_hex_byte((char *)p + 8);
            int lo = parse_hex_byte((char *)p + 10);
            base_addr = ((hi << 8) | lo) << 4;
            break;
        }
        case 0x04: {
            /* Extended linear address */
            if (byte_count != 2) {
                fprintf(stderr, "ihex: line %d: bad ext lin addr\n", line_num);
                return -1;
            }
            int hi = parse_hex_byte((char *)p + 8);
            int lo = parse_hex_byte((char *)p + 10);
            base_addr = ((uint32_t)(hi << 8) | lo) << 16;
            break;
        }
        default:
            /* Ignore unknown record types */
            break;
        }
    }

    return 0; /* EOF without explicit end record — still OK */
}

int ihex_load(const char *filename, uint16_t *flash, uint32_t flash_words)
{
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "ihex: cannot open '%s'\n", filename);
        return -1;
    }
    int ret = ihex_load_fp(fp, flash, flash_words);
    fclose(fp);
    return ret;
}

/* ---------- Byte-addressed loader (for 8051 code memory) ---------- */

static int ihex_load_bytes_fp(FILE *fp, uint8_t *buf, uint32_t buf_size)
{
    char line[600];
    uint32_t base_addr = 0;
    int line_num = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        char *p = line;
        size_t len = strlen(p);
        while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r'))
            p[--len] = '\0';
        if (len == 0) continue;
        if (p[0] != ':') continue;
        p++; len--;
        if (len < 10) continue;

        int byte_count = parse_hex_byte(p);
        int addr_hi    = parse_hex_byte(p + 2);
        int addr_lo    = parse_hex_byte(p + 4);
        int rec_type   = parse_hex_byte(p + 6);
        if (byte_count < 0 || addr_hi < 0 || addr_lo < 0 || rec_type < 0)
            continue;

        uint16_t addr = (addr_hi << 8) | addr_lo;

        switch (rec_type) {
        case 0x00:
            for (int i = 0; i < byte_count; i++) {
                int b = parse_hex_byte((char *)p + 8 + i * 2);
                if (b < 0) break;
                uint32_t byte_addr = base_addr + addr + i;
                if (byte_addr < buf_size)
                    buf[byte_addr] = (uint8_t)b;
            }
            break;
        case 0x01:
            return 0;
        case 0x02: {
            int hi = parse_hex_byte((char *)p + 8);
            int lo = parse_hex_byte((char *)p + 10);
            base_addr = ((hi << 8) | lo) << 4;
            break;
        }
        case 0x04: {
            int hi = parse_hex_byte((char *)p + 8);
            int lo = parse_hex_byte((char *)p + 10);
            base_addr = ((uint32_t)(hi << 8) | lo) << 16;
            break;
        }
        }
    }
    return 0;
}

int ihex_load_bytes(const char *filename, uint8_t *buf, uint32_t buf_size)
{
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "ihex: cannot open '%s'\n", filename);
        return -1;
    }
    int ret = ihex_load_bytes_fp(fp, buf, buf_size);
    fclose(fp);
    return ret;
}
