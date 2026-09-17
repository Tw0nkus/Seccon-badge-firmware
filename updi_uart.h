#ifndef UPDI_UART_H
#define UPDI_UART_H

#include "updi.h"

#define UPDI_UART_TX_PIN 16
#define UPDI_UART_RX_PIN 17

void updi_uart_init(void);
void updi_uart_deinit(void);

extern const updi_phy_t UPDI_UART_PHY;

#endif
