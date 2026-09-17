#include "updi_uart.h"

#include "badge_files.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "hardware/structs/uart.h"
#include "pico/time.h"

#define UPDI_UART uart0

static void uart_reset_format(uint32_t baud, uint stop_bits) {
    uart_init(UPDI_UART, baud);
    gpio_set_function(UPDI_UART_TX_PIN,
                      UART_FUNCSEL_NUM(UPDI_UART, UPDI_UART_TX_PIN));
    gpio_set_function(UPDI_UART_RX_PIN,
                      UART_FUNCSEL_NUM(UPDI_UART, UPDI_UART_RX_PIN));
    uart_set_hw_flow(UPDI_UART, false, false);
    uart_set_format(UPDI_UART, 8, stop_bits, UART_PARITY_EVEN);
    uart_set_fifo_enabled(UPDI_UART, true);
}

void updi_uart_init(void) {
    uart_reset_format(115200, 2);
}

void updi_uart_deinit(void) {
    uart_deinit(UPDI_UART);
    gpio_init(UPDI_UART_TX_PIN);
    gpio_set_dir(UPDI_UART_TX_PIN, GPIO_IN);
    gpio_disable_pulls(UPDI_UART_TX_PIN);
    gpio_init(UPDI_UART_RX_PIN);
    gpio_set_dir(UPDI_UART_RX_PIN, GPIO_IN);
    gpio_disable_pulls(UPDI_UART_RX_PIN);
}

static bool drain_rx_byte_or_error(void) {
    if (uart_is_readable(UPDI_UART)) {
        uart_getc(UPDI_UART);
        return true;
    }
    return false;
}

static void phy_flush_rx(void) {
    while (drain_rx_byte_or_error()) {}
}

static void phy_write_byte(uint8_t byte) {
    uart_putc_raw(UPDI_UART, byte);
}

static void phy_wait_tx_done(void) {
    uart_tx_wait_blocking(UPDI_UART);
}

static bool phy_read_byte_timeout(uint32_t timeout_us, uint8_t *out) {
    if (!uart_is_readable_within_us(UPDI_UART, timeout_us)) {
        return false;
    }
    *out = uart_getc(UPDI_UART);
    return true;
}

static void phy_set_baud(uint32_t baud, uint8_t stop_bits) {
    uart_reset_format(baud, stop_bits);
    while (drain_rx_byte_or_error()) {}
}

static void phy_send_break(void) {
    hw_set_bits(&uart_get_hw(UPDI_UART)->lcr_h, UART_UARTLCR_H_BRK_BITS);
    sleep_ms(2);
    hw_clear_bits(&uart_get_hw(UPDI_UART)->lcr_h, UART_UARTLCR_H_BRK_BITS);
}

static void phy_delay_ms(uint32_t ms) {
    while (ms > 5) {
        sleep_ms(5);
        badge_files_task();
        ms -= 5;
    }
    sleep_ms(ms);
    badge_files_task();
}

static void phy_deinit(void) {
    uart_deinit(UPDI_UART);
}

const updi_phy_t UPDI_UART_PHY = {
    .flush_rx = phy_flush_rx,
    .write_byte = phy_write_byte,
    .wait_tx_done = phy_wait_tx_done,
    .read_byte_timeout = phy_read_byte_timeout,
    .send_break = phy_send_break,
    .set_baud = phy_set_baud,
    .delay_ms = phy_delay_ms,
    .deinit = phy_deinit,
};
