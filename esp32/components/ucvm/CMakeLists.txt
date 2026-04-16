set(UCVM_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src")

set(UCVM_SRCS
    "${UCVM_SRC_DIR}/io/io_bridge.c"
    "${UCVM_SRC_DIR}/gdb/gdb_stub.c"
    "${UCVM_SRC_DIR}/util/ihex.c"
)

# AVR sources (conditional on Kconfig)
if(CONFIG_UCVM_ENABLE_AVR)
    list(APPEND UCVM_SRCS
        "${UCVM_SRC_DIR}/avr/avr_cpu.c"
        "${UCVM_SRC_DIR}/avr/avr_decode.c"
        "${UCVM_SRC_DIR}/avr/avr_timer.c"
        "${UCVM_SRC_DIR}/avr/avr_gpio.c"
        "${UCVM_SRC_DIR}/avr/avr_uart.c"
        "${UCVM_SRC_DIR}/avr/avr_twi.c"
        "${UCVM_SRC_DIR}/avr/avr_mcu_ops.c"
        "${UCVM_SRC_DIR}/avr/atmega328p.c"
        "${UCVM_SRC_DIR}/avr/attiny85.c"
        "${UCVM_SRC_DIR}/gdb/gdb_target_avr.c"
    )
endif()

# MCS-51 (8051) sources (conditional on Kconfig)
if(CONFIG_UCVM_ENABLE_MCS51)
    list(APPEND UCVM_SRCS
        "${UCVM_SRC_DIR}/mcs51/mcs51_cpu.c"
        "${UCVM_SRC_DIR}/mcs51/mcs51_decode.c"
        "${UCVM_SRC_DIR}/mcs51/mcs51_timer.c"
        "${UCVM_SRC_DIR}/mcs51/mcs51_uart.c"
        "${UCVM_SRC_DIR}/mcs51/mcs51_gpio.c"
        "${UCVM_SRC_DIR}/mcs51/mcs51_mcu_ops.c"
        "${UCVM_SRC_DIR}/mcs51/at89s52.c"
        "${UCVM_SRC_DIR}/gdb/gdb_target_mcs51.c"
    )
endif()

idf_component_register(
    SRCS
        ${UCVM_SRCS}
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../.."
    PRIV_REQUIRES
        spiffs
        nvs_flash
        lwip
)
