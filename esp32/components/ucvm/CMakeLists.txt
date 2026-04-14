set(UCVM_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src")
set(UCVM_VAR_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../variants")

idf_component_register(
    SRCS
        "${UCVM_SRC_DIR}/core/avr_cpu.c"
        "${UCVM_SRC_DIR}/core/avr_decode.c"
        "${UCVM_SRC_DIR}/periph/avr_timer.c"
        "${UCVM_SRC_DIR}/periph/avr_gpio.c"
        "${UCVM_SRC_DIR}/periph/avr_uart.c"
        "${UCVM_SRC_DIR}/io/io_bridge.c"
        "${UCVM_SRC_DIR}/gdb/gdb_stub.c"
        "${UCVM_SRC_DIR}/util/ihex.c"
        "${UCVM_VAR_DIR}/atmega328p.c"
        "${UCVM_VAR_DIR}/attiny85.c"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../.."
    PRIV_REQUIRES
        spiffs
        nvs_flash
        lwip
)
