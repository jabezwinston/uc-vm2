/*
 * ucvm - I/O Bridge implementation
 */
#include "io_bridge.h"
#include <string.h>

int io_bridge_init(io_bridge_t *br, void *cpu, const io_mcu_ops_t *mcu_ops)
{
    memset(br, 0, sizeof(*br));
    br->cpu     = cpu;
    br->mcu_ops = mcu_ops;
    return 0;
}

int io_bridge_add(io_bridge_t *br, const io_bridge_entry_t *entry)
{
    if (br->num_entries >= IO_BRIDGE_MAX_ENTRIES)
        return -1;
    br->entries[br->num_entries++] = *entry;
    return br->num_entries - 1;
}

int io_bridge_remove(io_bridge_t *br, int index)
{
    if (index < 0 || index >= br->num_entries)
        return -1;
    for (int i = index; i < br->num_entries - 1; i++)
        br->entries[i] = br->entries[i + 1];
    br->num_entries--;
    return 0;
}

/* ---- Binary config format ----
 * Header: "UCIO" (4) + version (1) + count (1) + reserved (2) = 8 bytes
 * Body:   count * io_bridge_entry_t (8 bytes each)                       */

#define IO_MAGIC   "UCIO"
#define IO_VERSION 3

int io_bridge_serialize(const io_bridge_t *br, uint8_t *buf, uint32_t len)
{
    uint32_t need = 8 + br->num_entries * sizeof(io_bridge_entry_t);
    if (len < need)
        return -1;

    memcpy(buf, IO_MAGIC, 4);
    buf[4] = IO_VERSION;
    buf[5] = br->num_entries;
    buf[6] = 0;
    buf[7] = 0;
    memcpy(buf + 8, br->entries,
           br->num_entries * sizeof(io_bridge_entry_t));
    return (int)need;
}

int io_bridge_parse(const uint8_t *data, uint32_t len, io_bridge_t *br)
{
    if (len < 8)
        return -1;
    if (memcmp(data, IO_MAGIC, 4) != 0)
        return -1;
    if (data[4] != IO_VERSION)
        return -1;

    uint8_t count = data[5];
    if (count > IO_BRIDGE_MAX_ENTRIES)
        count = IO_BRIDGE_MAX_ENTRIES;
    if (len < 8 + count * sizeof(io_bridge_entry_t))
        return -1;

    memcpy(br->entries, data + 8,
           count * sizeof(io_bridge_entry_t));
    br->num_entries = count;
    return 0;
}
