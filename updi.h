#ifndef UPDI_H
#define UPDI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPDI_DEVICE_COUNT 94

typedef enum {
    UPDI_OK,
    UPDI_TIMEOUT,
    UPDI_ECHO_ERROR,
    UPDI_ACK_ERROR,
    UPDI_PROTOCOL_ERROR,
    UPDI_LOCKED,
    UPDI_NVM_ERROR,
    UPDI_UNSUPPORTED_DEVICE,
    UPDI_WRONG_DEVICE,
    UPDI_HEX_ERROR,
    UPDI_RANGE_ERROR,
} updi_status_t;

const char *updi_status_str(updi_status_t status);

void updi_set_trace(void (*trace_fn)(const char *msg));

typedef struct {
    const char *name;
    uint8_t signature[3];
    uint32_t flash_start;
    uint32_t flash_size;
    uint16_t page_size;
    uint16_t signature_address;
    uint16_t nvmctrl_address;
    uint16_t syscfg_address;
    uint8_t address_bits;
    uint16_t flash_write_size_override;
} updi_device_t;

extern const updi_device_t UPDI_DEVICES[UPDI_DEVICE_COUNT];

typedef struct {
    const char *name;
    uint8_t signature[3];
    uint8_t revision;
    uint32_t flash_start;
    uint32_t flash_size;
    uint16_t page_size;
    uint16_t flash_write_size;
    uint8_t nvm_version;
} updi_target_info_t;

typedef struct {
    void (*flush_rx)(void);
    void (*write_byte)(uint8_t byte);
    void (*wait_tx_done)(void);
    bool (*read_byte_timeout)(uint32_t timeout_us, uint8_t *out);
    void (*send_break)(void);
    void (*set_baud)(uint32_t baud, uint8_t stop_bits);
    void (*delay_ms)(uint32_t ms);
    void (*deinit)(void);
} updi_phy_t;

updi_status_t updi_ping(const updi_phy_t *phy, const updi_device_t *device,
                        updi_target_info_t *out);
updi_status_t updi_identify(const updi_phy_t *phy, updi_target_info_t *out,
                            const updi_device_t **matched);
updi_status_t updi_erase(const updi_phy_t *phy, const updi_device_t *device);
updi_status_t updi_flash_hex(const updi_phy_t *phy, const updi_device_t *device,
                             const uint8_t *hex, size_t hex_len,
                             bool erase_first, bool verify);

size_t updi_trim_hex_len(const uint8_t *data, size_t len);

#endif

