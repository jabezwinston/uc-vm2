/*
 * ucvm - Microcontroller Virtual Machine
 * AVR peripheral emulation: Timer0, GPIO, UART
 */
#ifndef AVR_PERIPH_H
#define AVR_PERIPH_H

#include <stdint.h>

/* Forward declaration */
typedef struct avr_cpu avr_cpu_t;

/* ========== Timer0 ========== */

/* Timer0 modes */
enum {
    TIMER0_MODE_NORMAL   = 0,
    TIMER0_MODE_PWM_PC   = 1,  /* Phase Correct PWM */
    TIMER0_MODE_CTC      = 2,
    TIMER0_MODE_FAST_PWM = 3,
};

/* Clock select */
enum {
    TIMER0_CS_STOP = 0,
    TIMER0_CS_1    = 1,
    TIMER0_CS_8    = 2,
    TIMER0_CS_64   = 3,
    TIMER0_CS_256  = 4,
    TIMER0_CS_1024 = 5,
    TIMER0_CS_EXT_FALL = 6,
    TIMER0_CS_EXT_RISE = 7,
};

/* I/O addresses for Timer0 (variant-specific) */
typedef struct {
    uint8_t tccr0a;   /* TCCR0A I/O index (data_addr - 0x20) */
    uint8_t tccr0b;   /* TCCR0B */
    uint8_t tcnt0;    /* TCNT0 */
    uint8_t ocr0a;    /* OCR0A */
    uint8_t ocr0b;    /* OCR0B */
    uint8_t timsk;    /* TIMSK0 (328P) or TIMSK (tiny85) */
    uint8_t tifr;     /* TIFR0 (328P) or TIFR (tiny85) */
    uint8_t toie_bit; /* TOV interrupt enable bit in TIMSK */
    uint8_t ocie0a_bit; /* OCF0A int enable bit in TIMSK */
    uint8_t tov_vec;  /* Timer overflow interrupt vector number */
    uint8_t oca_vec;  /* Timer compare A interrupt vector number */
} avr_timer0_config_t;

typedef struct {
    const avr_timer0_config_t *config;
    uint16_t prescaler_count; /* cycles accumulated toward next timer tick */
    uint8_t  counting_up;     /* for phase-correct PWM */
} avr_timer0_t;

/* Init Timer0 — registers I/O handlers on cpu */
avr_timer0_t *avr_timer0_init(avr_cpu_t *cpu, const avr_timer0_config_t *config);

/* Tick Timer0 by elapsed CPU cycles */
void avr_timer0_tick(avr_cpu_t *cpu, avr_timer0_t *timer, uint8_t cycles);

/* ========== GPIO ========== */

/* GPIO port config (one per port: B, C, D) */
typedef struct {
    uint8_t pin_io;   /* PINx I/O index */
    uint8_t ddr_io;   /* DDRx I/O index */
    uint8_t port_io;  /* PORTx I/O index */
    uint8_t port_id;  /* port identifier (0=B, 1=C, 2=D) */
    uint8_t num_pins; /* number of pins on this port */
} avr_gpio_port_config_t;

typedef struct {
    const avr_gpio_port_config_t *ports;
    uint8_t num_ports;
    uint8_t ext_pins[8];  /* externally driven pin values per port (max 8 ports) */
} avr_gpio_t;

avr_gpio_t *avr_gpio_init(avr_cpu_t *cpu,
                           const avr_gpio_port_config_t *ports, uint8_t num_ports);

/* ========== UART ========== */

#define UART_BUF_SIZE 32

typedef struct {
    uint8_t udr_io;     /* UDR0 I/O index */
    uint8_t ucsra_io;   /* UCSR0A */
    uint8_t ucsrb_io;   /* UCSR0B */
    uint8_t ucsrc_io;   /* UCSR0C */
    uint8_t ubrrl_io;   /* UBRR0L */
    uint8_t ubrrh_io;   /* UBRR0H */
    uint8_t rxc_vec;    /* RX complete interrupt vector */
    uint8_t udre_vec;   /* Data register empty interrupt vector */
    uint8_t txc_vec;    /* TX complete interrupt vector */
} avr_uart_config_t;

typedef struct {
    const avr_uart_config_t *config;
    /* TX ring buffer */
    uint8_t tx_buf[UART_BUF_SIZE];
    uint8_t tx_head;
    uint8_t tx_tail;
    /* RX ring buffer */
    uint8_t rx_buf[UART_BUF_SIZE];
    uint8_t rx_head;
    uint8_t rx_tail;
} avr_uart_t;

avr_uart_t *avr_uart_init(avr_cpu_t *cpu, const avr_uart_config_t *config);

/* Push a byte into the UART RX buffer (from external source) */
void avr_uart_rx_push(avr_cpu_t *cpu, avr_uart_t *uart, uint8_t byte);

/* Pop a byte from the UART TX buffer (to external sink).
 * Returns -1 if empty. */
int avr_uart_tx_pop(avr_uart_t *uart);

/* ========== TWI (I2C) ========== */

/* I2C bus operations — called by TWI peripheral during emulated bus transactions.
 * Attach a bus to forward transactions to real hardware (ESP32) or virtual slaves (PC). */
typedef struct avr_twi_bus {
    int  (*start)(void *ctx);                        /* Bus START. Returns 0=ok. */
    int  (*write_byte)(void *ctx, uint8_t byte);     /* TX byte. Returns 1=ACK, 0=NACK. */
    int  (*read_byte)(void *ctx, int send_ack);      /* RX byte. Returns 0-255, or -1. */
    void (*stop)(void *ctx);                         /* Bus STOP. */
    void *ctx;
} avr_twi_bus_t;

/* TWI register config (variant-specific I/O indices = data_addr - 0x20) */
typedef struct {
    uint8_t twbr_io;    /* TWBR  */
    uint8_t twsr_io;    /* TWSR  */
    uint8_t twar_io;    /* TWAR  */
    uint8_t twdr_io;    /* TWDR  */
    uint8_t twcr_io;    /* TWCR  */
    uint8_t twamr_io;   /* TWAMR */
    uint8_t twi_vec;    /* TWI interrupt vector number */
} avr_twi_config_t;

/* TWI bus states */
enum {
    TWI_IDLE = 0,
    TWI_START_SENT,         /* START completed, waiting for SLA+R/W in TWDR */
    TWI_MT_ADDR_SENT,       /* SLA+W sent, ACK/NACK received */
    TWI_MT_DATA_SENT,       /* Data byte sent in MT mode */
    TWI_MR_ADDR_SENT,       /* SLA+R sent, ACK/NACK received */
    TWI_MR_DATA_RECEIVED,   /* Data byte received in MR mode */
};

/* TWI peripheral state */
typedef struct {
    const avr_twi_config_t *config;
    avr_twi_bus_t *bus;         /* Attached I2C bus (NULL = no device, NACK everything) */

    uint8_t  bus_state;         /* TWI_IDLE, TWI_START_SENT, etc. */
    uint8_t  status;            /* Current TWSR status code (top 5 bits) */
    uint8_t  slave_rw;          /* 0=write, 1=read (from SLA+R/W) */

    /* Cycle-accurate timing */
    uint32_t cycles_remaining;  /* CPU cycles until current operation completes */
    uint8_t  pending_op;        /* Operation in progress (0=none) */
    uint8_t  pending_data;      /* Data byte for pending operation */
    uint8_t  pending_ack;       /* ACK flag for pending read */
} avr_twi_t;

/* Pending operation codes */
enum {
    TWI_OP_NONE = 0,
    TWI_OP_START,
    TWI_OP_SLA_W,
    TWI_OP_SLA_R,
    TWI_OP_DATA_TX,
    TWI_OP_DATA_RX,
    TWI_OP_STOP,
};

/* Init TWI — registers I/O handlers on cpu. Returns state stored in cpu->periph_twi. */
avr_twi_t *avr_twi_init(avr_cpu_t *cpu, const avr_twi_config_t *config);

/* Tick TWI by elapsed CPU cycles — call from cpu_step */
void avr_twi_tick(avr_cpu_t *cpu, avr_twi_t *twi, uint8_t cycles);

/* Attach/detach an I2C bus (for ESP32 bridge or virtual slave) */
void avr_twi_set_bus(avr_twi_t *twi, avr_twi_bus_t *bus);

/* Create a virtual I2C slave at given 7-bit address (for PC testing).
 * Returns a bus handle to pass to avr_twi_set_bus(). */
avr_twi_bus_t *avr_twi_create_virtual_slave(uint8_t addr, int verbose);
uint8_t *avr_twi_virtual_slave_regs(void);

#endif /* AVR_PERIPH_H */
