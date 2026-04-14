/*
 * ucvm - GDB target ops for AVR
 */
#include "gdb_target.h"
#include "gdb_stub.h"
#include "../avr/avr_cpu.h"
#include <string.h>

static const char hx[] = "0123456789abcdef";
static void hb(char *o, uint8_t v) { o[0]=hx[(v>>4)&0xF]; o[1]=hx[v&0xF]; }
static int hv(char c) {
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    if(c>='A'&&c<='F') return c-'A'+10;
    return -1;
}

static int avr_read_regs(void *cpu_v, char *buf, int buf_size)
{
    avr_cpu_t *cpu = cpu_v;
    char *p = buf;
    (void)buf_size;
    for (int i = 0; i < 32; i++) { hb(p, AVR_R(cpu, i)); p += 2; }
    hb(p, cpu->sreg); p += 2;
    hb(p, cpu->sp & 0xFF); p += 2;
    hb(p, (cpu->sp >> 8) & 0xFF); p += 2;
    uint32_t pc_bytes = (uint32_t)cpu->pc * 2;
    hb(p, pc_bytes & 0xFF); p += 2;
    hb(p, (pc_bytes >> 8) & 0xFF); p += 2;
    hb(p, (pc_bytes >> 16) & 0xFF); p += 2;
    hb(p, (pc_bytes >> 24) & 0xFF); p += 2;
    *p = '\0';
    return (int)(p - buf);
}

static void avr_write_regs(void *cpu_v, const char *data)
{
    avr_cpu_t *cpu = cpu_v;
    const char *p = data;
    for (int i = 0; i < 32 && *p && *(p+1); i++) {
        AVR_R(cpu, i) = (hv(p[0]) << 4) | hv(p[1]); p += 2;
    }
    if (*p && *(p+1)) { cpu->sreg = (hv(p[0]) << 4) | hv(p[1]); p += 2; }
    if (*p && *(p+1)) {
        uint8_t spl = (hv(p[0]) << 4) | hv(p[1]); p += 2;
        if (*p && *(p+1)) {
            uint8_t sph = (hv(p[0]) << 4) | hv(p[1]); p += 2;
            cpu->sp = (sph << 8) | spl;
        }
    }
    if (*p && *(p+1)) {
        uint32_t pc_bytes = 0;
        for (int i = 0; i < 4 && *p && *(p+1); i++) {
            pc_bytes |= ((uint32_t)((hv(p[0]) << 4) | hv(p[1]))) << (i * 8);
            p += 2;
        }
        cpu->pc = pc_bytes / 2;
    }
}

static uint8_t avr_read_mem(void *cpu_v, uint32_t addr)
{
    avr_cpu_t *cpu = cpu_v;
    if (addr < 0x800000)
        return avr_flash_read_byte(cpu, (uint16_t)addr);
    if (addr < 0x810000)
        return avr_data_read(cpu, (uint16_t)(addr - 0x800000));
    return 0;
}

static void avr_write_mem(void *cpu_v, uint32_t addr, uint8_t val)
{
    avr_cpu_t *cpu = cpu_v;
    if (addr >= 0x800000 && addr < 0x810000)
        avr_data_write(cpu, (uint16_t)(addr - 0x800000), val);
}

static uint32_t avr_get_pc(void *cpu_v)
{
    return ((avr_cpu_t *)cpu_v)->pc * 2; /* Return byte address */
}

static void avr_set_pc(void *cpu_v, uint32_t addr)
{
    ((avr_cpu_t *)cpu_v)->pc = addr / 2; /* From byte to word address */
}

static uint8_t avr_get_state(void *cpu_v) { return ((avr_cpu_t *)cpu_v)->state; }
static void avr_set_state(void *cpu_v, uint8_t s) { ((avr_cpu_t *)cpu_v)->state = s; }
static uint8_t avr_step(void *cpu_v) { return avr_cpu_step(cpu_v); }
static void avr_reset(void *cpu_v) { avr_cpu_reset(cpu_v); }

/* Legacy AVR init wrapper */
gdb_state_t *gdb_init_avr(void *cpu, const uint16_t *flash,
                            uint32_t flash_words, int port)
{
    (void)flash; (void)flash_words;
    return gdb_init(cpu, &gdb_target_avr, port);
}

const gdb_target_ops_t gdb_target_avr = {
    .read_regs  = avr_read_regs,
    .write_regs = avr_write_regs,
    .read_mem   = avr_read_mem,
    .write_mem  = avr_write_mem,
    .get_pc     = avr_get_pc,
    .set_pc     = avr_set_pc,
    .get_state  = avr_get_state,
    .set_state  = avr_set_state,
    .step       = avr_step,
    .reset      = avr_reset,
};
