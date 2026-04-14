/*
 * ucvm - GDB target ops for MCS-51 (8051)
 *
 * Register layout for RSP:
 *   PC (2 bytes LE) + ACC (1) + B (1) + PSW (1) + SP (1) + DPL (1) + DPH (1) + R0-R7 (8)
 *   Total: 16 bytes = 32 hex chars
 *
 * Memory mapping:
 *   0x000000-0x00FFFF: Code space
 *   0x0D0000-0x0D00FF: Internal RAM (IRAM, direct addressing view)
 *   0x0F0000-0x0FFFFF: External RAM (XDATA)
 *   For simplicity, also accept raw addresses < 0x10000 as code space.
 */
#include "gdb_target.h"
#include "../mcs51/mcs51_cpu.h"

static const char hx[] = "0123456789abcdef";
static void hb(char *o, uint8_t v) { o[0]=hx[(v>>4)&0xF]; o[1]=hx[v&0xF]; }
static int hv(char c) {
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    if(c>='A'&&c<='F') return c-'A'+10;
    return -1;
}

static int mcs51_read_regs(void *cpu_v, char *buf, int buf_size)
{
    mcs51_cpu_t *cpu = cpu_v;
    char *p = buf;
    (void)buf_size;
    /* PC: 2 bytes, little-endian */
    hb(p, cpu->pc & 0xFF); p += 2;
    hb(p, (cpu->pc >> 8) & 0xFF); p += 2;
    /* ACC, B, PSW, SP, DPL, DPH */
    hb(p, ACC(cpu)); p += 2;
    hb(p, B(cpu)); p += 2;
    hb(p, PSW(cpu)); p += 2;
    hb(p, SP(cpu)); p += 2;
    hb(p, DPL(cpu)); p += 2;
    hb(p, DPH(cpu)); p += 2;
    /* R0-R7 from current register bank */
    for (int i = 0; i < 8; i++) { hb(p, REG(cpu, i)); p += 2; }
    *p = '\0';
    return (int)(p - buf);
}

static void mcs51_write_regs(void *cpu_v, const char *data)
{
    mcs51_cpu_t *cpu = cpu_v;
    const char *p = data;
    /* PC */
    if (*p && *(p+1)) {
        uint16_t pcl = (hv(p[0]) << 4) | hv(p[1]); p += 2;
        if (*p && *(p+1)) {
            uint16_t pch = (hv(p[0]) << 4) | hv(p[1]); p += 2;
            cpu->pc = (pch << 8) | pcl;
        }
    }
    /* ACC, B, PSW, SP, DPL, DPH */
    if (*p && *(p+1)) { ACC(cpu) = (hv(p[0]) << 4) | hv(p[1]); p += 2; }
    if (*p && *(p+1)) { B(cpu) = (hv(p[0]) << 4) | hv(p[1]); p += 2; }
    if (*p && *(p+1)) { PSW(cpu) = (hv(p[0]) << 4) | hv(p[1]); p += 2; }
    if (*p && *(p+1)) { SP(cpu) = (hv(p[0]) << 4) | hv(p[1]); p += 2; }
    if (*p && *(p+1)) { DPL(cpu) = (hv(p[0]) << 4) | hv(p[1]); p += 2; }
    if (*p && *(p+1)) { DPH(cpu) = (hv(p[0]) << 4) | hv(p[1]); p += 2; }
    /* R0-R7 */
    for (int i = 0; i < 8 && *p && *(p+1); i++) {
        REG(cpu, i) = (hv(p[0]) << 4) | hv(p[1]); p += 2;
    }
}

static uint8_t mcs51_read_mem(void *cpu_v, uint32_t addr)
{
    mcs51_cpu_t *cpu = cpu_v;
    if (addr < 0x10000)
        return mcs51_code_read(cpu, (uint16_t)addr);
    if (addr >= 0x0D0000 && addr < 0x0D0100)
        return mcs51_direct_read(cpu, (uint8_t)(addr - 0x0D0000));
    if (addr >= 0x0F0000 && addr < 0x100000)
        return mcs51_xdata_read(cpu, (uint16_t)(addr - 0x0F0000));
    return 0xFF;
}

static void mcs51_write_mem(void *cpu_v, uint32_t addr, uint8_t val)
{
    mcs51_cpu_t *cpu = cpu_v;
    if (addr >= 0x0D0000 && addr < 0x0D0100)
        mcs51_direct_write(cpu, (uint8_t)(addr - 0x0D0000), val);
    else if (addr >= 0x0F0000 && addr < 0x100000)
        mcs51_xdata_write(cpu, (uint16_t)(addr - 0x0F0000), val);
    /* Code space writes ignored */
}

static uint32_t mcs51_get_pc(void *cpu_v) { return ((mcs51_cpu_t *)cpu_v)->pc; }
static void mcs51_set_pc(void *cpu_v, uint32_t addr) { ((mcs51_cpu_t *)cpu_v)->pc = (uint16_t)addr; }
static uint8_t mcs51_get_state(void *cpu_v) { return ((mcs51_cpu_t *)cpu_v)->state; }
static void mcs51_set_state(void *cpu_v, uint8_t s) { ((mcs51_cpu_t *)cpu_v)->state = s; }
static uint8_t mcs51_step(void *cpu_v) { return mcs51_cpu_step(cpu_v); }
static void mcs51_reset(void *cpu_v) { mcs51_cpu_reset(cpu_v); }

const gdb_target_ops_t gdb_target_mcs51 = {
    .read_regs  = mcs51_read_regs,
    .write_regs = mcs51_write_regs,
    .read_mem   = mcs51_read_mem,
    .write_mem  = mcs51_write_mem,
    .get_pc     = mcs51_get_pc,
    .set_pc     = mcs51_set_pc,
    .get_state  = mcs51_get_state,
    .set_state  = mcs51_set_state,
    .step       = mcs51_step,
    .reset      = mcs51_reset,
};
