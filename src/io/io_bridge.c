/*
 * ucvm - I/O Bridge Framework implementation
 */
#include "io_bridge.h"
#include <string.h>

int io_bridge_parse(const uint8_t *data, uint32_t len, io_bridge_config_t *out)
{
    if (len < sizeof(io_bridge_header_t))
        return -1;

    const io_bridge_header_t *hdr = (const io_bridge_header_t *)data;

    /* Validate magic */
    if (memcmp(hdr->magic, IO_BRIDGE_MAGIC, 4) != 0)
        return -1;

    /* Validate version */
    if (hdr->version != IO_BRIDGE_VERSION)
        return -1;

    uint8_t count = hdr->num_entries;
    if (count > IO_BRIDGE_MAX_ENTRIES)
        count = IO_BRIDGE_MAX_ENTRIES;

    uint32_t expected = sizeof(io_bridge_header_t) + count * sizeof(io_bridge_entry_t);
    if (len < expected)
        return -1;

    const io_bridge_entry_t *entries =
        (const io_bridge_entry_t *)(data + sizeof(io_bridge_header_t));

    memcpy(out->entries, entries, count * sizeof(io_bridge_entry_t));
    out->num_entries = count;
    return 0;
}

int io_bridge_serialize(const io_bridge_config_t *config, uint8_t *buf, uint32_t buf_len)
{
    uint32_t needed = sizeof(io_bridge_header_t) +
                      config->num_entries * sizeof(io_bridge_entry_t);
    if (buf_len < needed)
        return -1;

    io_bridge_header_t *hdr = (io_bridge_header_t *)buf;
    memcpy(hdr->magic, IO_BRIDGE_MAGIC, 4);
    hdr->version = IO_BRIDGE_VERSION;
    hdr->num_entries = config->num_entries;

    memcpy(buf + sizeof(io_bridge_header_t),
           config->entries,
           config->num_entries * sizeof(io_bridge_entry_t));

    return (int)needed;
}

const io_bridge_entry_t *io_bridge_find(const io_bridge_config_t *config,
                                         uint8_t type, uint8_t avr_resource)
{
    for (uint8_t i = 0; i < config->num_entries; i++) {
        if (config->entries[i].type == type &&
            config->entries[i].avr_resource == avr_resource)
            return &config->entries[i];
    }
    return NULL;
}
