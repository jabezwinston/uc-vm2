/*
 * ucvm GDB stub test program
 *
 * Exercises: all integer types, float, arrays, structs, unions, enums,
 * pointers, pointer arithmetic, function pointers, recursion, nested calls,
 * volatile, const, static, globals, locals, bitfields.
 *
 * Compiled with -g -O0 for full debug info.
 * Designed to be stepped through and inspected by GDB.
 */
#include <avr/io.h>
#include <stdint.h>
#include <string.h>

/* ---- Marker: GDB reads this to confirm program is running ---- */
volatile uint8_t gdb_marker = 0;

/* ---- All integer types ---- */
uint8_t  g_u8  = 0x42;
uint16_t g_u16 = 0xBEEF;
uint32_t g_u32 = 0xDEADCAFE;
int8_t   g_i8  = -42;
int16_t  g_i16 = -1234;
int32_t  g_i32 = -100000;
char     g_char = 'Z';

/* ---- Struct with various fields ---- */
typedef struct {
    uint8_t  id;
    uint16_t value;
    int32_t  offset;
    char     name[8];
} sensor_t;

static sensor_t g_sensor = { .id = 7, .value = 1023, .offset = -50, .name = "TEMP" };

/* ---- Union ---- */
typedef union {
    uint32_t u32;
    uint16_t u16[2];
    uint8_t  u8[4];
} multi_t;

/* ---- Enum ---- */
typedef enum {
    STATE_IDLE = 0,
    STATE_RUNNING = 1,
    STATE_ERROR = 2,
    STATE_DONE = 3,
} state_t;

static volatile state_t g_state = STATE_IDLE;

/* ---- Bitfield struct ---- */
typedef struct {
    uint8_t enabled : 1;
    uint8_t mode    : 3;
    uint8_t channel : 4;
} config_bits_t;

/* ---- Function pointer type ---- */
typedef uint16_t (*math_fn_t)(uint16_t a, uint16_t b);

/* ---- Array of constants in flash (PROGMEM-like) ---- */
static const uint8_t lookup_table[16] = {
    0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 127, 255
};

/* ================ Functions ================ */

/* Simple add — verifies function call and return */
uint16_t add(uint16_t a, uint16_t b)
{
    return a + b;
}

/* Simple multiply */
uint16_t multiply(uint16_t a, uint16_t b)
{
    return a * b;
}

/* Recursive factorial — tests recursion, stack depth */
uint16_t factorial(uint8_t n)
{
    if (n <= 1)
        return 1;
    return n * factorial(n - 1);
}

/* Fibonacci — deeper recursion */
uint16_t fibonacci(uint8_t n)
{
    if (n <= 1)
        return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

/* Pointer manipulation — swap via pointers */
void swap(uint16_t *a, uint16_t *b)
{
    uint16_t tmp = *a;
    *a = *b;
    *b = tmp;
}

/* Struct pointer — modify through pointer */
void sensor_update(sensor_t *s, uint16_t new_val)
{
    s->value = new_val;
    s->offset += 1;
}

/* Array processing — sum with pointer arithmetic */
uint16_t array_sum(const uint8_t *arr, uint8_t len)
{
    uint16_t sum = 0;
    const uint8_t *end = arr + len;
    while (arr < end) {
        sum += *arr;
        arr++;
    }
    return sum;
}

/* Function pointer — call through pointer */
uint16_t apply_op(math_fn_t op, uint16_t x, uint16_t y)
{
    return op(x, y);
}

/* Nested function calls — exercises call stack */
uint16_t compute(uint8_t a, uint8_t b, uint8_t c)
{
    uint16_t ab = add(a, b);
    uint16_t abc = multiply(ab, c);
    return abc;
}

/* String operation on stack buffer */
uint8_t string_test(void)
{
    char buf[16];
    strcpy(buf, "Hello");
    strcat(buf, " GDB");
    return (uint8_t)strlen(buf);
}

/* Output a byte to PORTB — observable side effect */
static void output_byte(uint8_t val)
{
    PORTB = val;
}

/* ================ Main: structured test sequence ================ */

int main(void)
{
    /* Setup — set PB5 as output */
    DDRB = 0xFF;

    /* ---- CHECKPOINT 1: Basic types ---- */
    gdb_marker = 1;
    volatile uint8_t  local_u8  = 0xAA;
    volatile uint16_t local_u16 = 0x1234;
    volatile uint32_t local_u32 = 0xCAFEBABE;
    volatile int8_t   local_i8  = -100;
    volatile int16_t  local_i16 = 32000;
    volatile int32_t  local_i32 = 1000000;
    output_byte(0x01);

    /* ---- CHECKPOINT 2: Pointer operations ---- */
    gdb_marker = 2;
    uint16_t x = 100, y = 200;
    swap(&x, &y);
    /* After swap: x=200, y=100 */

    uint8_t *ptr = (uint8_t *)&local_u32;
    volatile uint8_t byte0 = ptr[0]; /* LSB of 0xCAFEBABE = 0xBE */
    volatile uint8_t byte1 = ptr[1]; /* 0xBA */
    volatile uint8_t byte2 = ptr[2]; /* 0xFE */
    volatile uint8_t byte3 = ptr[3]; /* 0xCA */
    output_byte(0x02);

    /* ---- CHECKPOINT 3: Struct and union ---- */
    gdb_marker = 3;
    sensor_update(&g_sensor, 512);
    /* g_sensor.value should be 512, offset should be -49 */

    multi_t m;
    m.u32 = 0x12345678;
    volatile uint8_t m_b0 = m.u8[0]; /* 0x78 (little-endian) */
    volatile uint8_t m_b3 = m.u8[3]; /* 0x12 */
    volatile uint16_t m_w0 = m.u16[0]; /* 0x5678 */

    config_bits_t cfg;
    cfg.enabled = 1;
    cfg.mode = 5;
    cfg.channel = 12;
    output_byte(0x03);

    /* ---- CHECKPOINT 4: Array and loops ---- */
    gdb_marker = 4;
    uint8_t arr[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };
    volatile uint16_t sum = array_sum(arr, 8);
    /* sum should be 360 */

    volatile uint16_t lookup_sum = array_sum(lookup_table, 16);
    output_byte(0x04);

    /* ---- CHECKPOINT 5: Function calls ---- */
    gdb_marker = 5;
    volatile uint16_t result_add = add(123, 456);       /* 579 */
    volatile uint16_t result_mul = multiply(12, 34);     /* 408 */
    volatile uint16_t result_comp = compute(3, 4, 5);    /* (3+4)*5 = 35 */
    output_byte(0x05);

    /* ---- CHECKPOINT 6: Recursion ---- */
    gdb_marker = 6;
    volatile uint16_t fact5 = factorial(5);    /* 120 */
    volatile uint16_t fib8  = fibonacci(8);    /* 21  */
    output_byte(0x06);

    /* ---- CHECKPOINT 7: Function pointers ---- */
    gdb_marker = 7;
    math_fn_t op = add;
    volatile uint16_t fp_result1 = apply_op(op, 100, 200); /* 300 */
    op = multiply;
    volatile uint16_t fp_result2 = apply_op(op, 10, 20);   /* 200 */
    output_byte(0x07);

    /* ---- CHECKPOINT 8: Enum and state ---- */
    gdb_marker = 8;
    g_state = STATE_RUNNING;
    volatile state_t local_state = g_state;
    g_state = STATE_DONE;
    output_byte(0x08);

    /* ---- CHECKPOINT 9: String operations ---- */
    gdb_marker = 9;
    volatile uint8_t str_len = string_test(); /* 9 = len("Hello GDB") */
    output_byte(0x09);

    /* ---- CHECKPOINT 10: All done — loop with marker ---- */
    gdb_marker = 10;
    output_byte(0xFF);

    /* Signal completion and halt */
    while (1) {
        gdb_marker = 0xFF;
        output_byte(gdb_marker);
    }

    return 0;
}
