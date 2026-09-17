#include "updi.h"

#include <stdio.h>
#include <string.h>

#define UPDI_LDS 0x00
#define UPDI_STS 0x40
#define UPDI_LD 0x20
#define UPDI_ST 0x60
#define UPDI_LDCS 0x80
#define UPDI_STCS 0xC0
#define UPDI_REPEAT 0xA0
#define UPDI_KEY 0xE0

#define UPDI_PTR_INC 0x04
#define UPDI_PTR_ADDRESS 0x08
#define UPDI_ADDRESS_16 0x04
#define UPDI_ADDRESS_24 0x08
#define UPDI_DATA_8 0x00
#define UPDI_DATA_16 0x01
#define UPDI_DATA_24 0x02
#define UPDI_KEY_KEY 0x00
#define UPDI_KEY_64 0x00
#define UPDI_KEY_SIB 0x04
#define UPDI_SIB_32BYTES 0x02
#define UPDI_REPEAT_BYTE 0x00
#define UPDI_PHY_SYNC 0x55
#define UPDI_PHY_ACK 0x40
#define UPDI_MAX_REPEAT_SIZE 0xFF

#define UPDI_CS_STATUSA 0x00
#define UPDI_CS_CTRLA 0x02
#define UPDI_CS_CTRLB 0x03
#define UPDI_ASI_KEY_STATUS 0x07
#define UPDI_ASI_RESET_REQ 0x08
#define UPDI_ASI_SYS_STATUS 0x0B

#define UPDI_CTRLA_IBDLY_BIT 7
#define UPDI_CTRLA_RSD_BIT 3
#define UPDI_CTRLB_CCDETDIS_BIT 3
#define UPDI_ASI_KEY_STATUS_NVMPROG 4
#define UPDI_ASI_SYS_STATUS_NVMPROG 3
#define UPDI_ASI_SYS_STATUS_LOCKSTATUS 0
#define UPDI_RESET_REQ_VALUE 0x59

#define UPDI_READ_TIMEOUT_US 1000000u
#define UPDI_MAX_PAGE_SIZE 512
#define UPDI_INIT_BAUD 57600u
#define UPDI_UART_BAUD 115200u

const char *updi_status_str(updi_status_t status) {
    switch (status) {
        case UPDI_OK: return "OK";
        case UPDI_TIMEOUT: return "Timeout";
        case UPDI_ECHO_ERROR: return "Echo error";
        case UPDI_ACK_ERROR: return "ACK error";
        case UPDI_PROTOCOL_ERROR: return "Protocol error";
        case UPDI_LOCKED: return "Target locked";
        case UPDI_NVM_ERROR: return "NVM error";
        case UPDI_UNSUPPORTED_DEVICE: return "Unsupported";
        case UPDI_WRONG_DEVICE: return "Wrong chip";
        case UPDI_HEX_ERROR: return "HEX error";
        case UPDI_RANGE_ERROR: return "Range error";
        default: return "Unknown";
    }
}

static void (*g_trace_fn)(const char *msg) = NULL;

void updi_set_trace(void (*trace_fn)(const char *msg)) {
    g_trace_fn = trace_fn;
}

static void trace(const char *msg) {
    if (g_trace_fn) g_trace_fn(msg);
}

static void trace_status(const char *label, updi_status_t status) {
    if (!g_trace_fn) return;
    char buf[80];
    snprintf(buf, sizeof(buf), "%s: %s", label, updi_status_str(status));
    g_trace_fn(buf);
}

static const updi_phy_t *g_phy;
static const updi_device_t *g_device;
static uint8_t g_address_bits;
static uint8_t g_nvm_version;

static updi_status_t updi_send(const uint8_t *data, size_t len) {
    g_phy->flush_rx();
    for (size_t i = 0; i < len; i++) {
        g_phy->write_byte(data[i]);
        g_phy->wait_tx_done();
        uint8_t echo;
        if (!g_phy->read_byte_timeout(UPDI_READ_TIMEOUT_US, &echo)) {
            if (g_trace_fn) {
                char buf[64];
                snprintf(buf, sizeof(buf), "send: byte %u/%u sent=0x%02X no echo (timeout)",
                         (unsigned)(i + 1), (unsigned)len, data[i]);
                g_trace_fn(buf);
            }
            return UPDI_TIMEOUT;
        }
        if (echo != data[i]) {
            if (g_trace_fn) {
                char buf[64];
                snprintf(buf, sizeof(buf), "send: byte %u/%u sent=0x%02X got=0x%02X",
                         (unsigned)(i + 1), (unsigned)len, data[i], echo);
                g_trace_fn(buf);
            }
            return UPDI_ECHO_ERROR;
        }
    }
    return UPDI_OK;
}

static updi_status_t updi_receive(uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (!g_phy->read_byte_timeout(UPDI_READ_TIMEOUT_US, &data[i])) {
            return UPDI_TIMEOUT;
        }
    }
    return UPDI_OK;
}

static updi_status_t updi_send_receive(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len) {
    updi_status_t status = updi_send(tx, tx_len);
    if (status != UPDI_OK) return status;
    return updi_receive(rx, rx_len);
}

static updi_status_t updi_stcs(uint8_t address, uint8_t value) {
    uint8_t frame[3] = {UPDI_PHY_SYNC, (uint8_t)(UPDI_STCS | (address & 0x0F)), value};
    return updi_send(frame, 3);
}

static updi_status_t updi_ldcs(uint8_t address, uint8_t *out) {
    uint8_t frame[2] = {UPDI_PHY_SYNC, (uint8_t)(UPDI_LDCS | (address & 0x0F))};
    return updi_send_receive(frame, 2, out, 1);
}

static updi_status_t updi_st(uint32_t address, uint8_t value) {
    uint8_t frame[5] = {
        UPDI_PHY_SYNC,
        (uint8_t)(UPDI_STS | (g_address_bits == 24 ? UPDI_ADDRESS_24 : UPDI_ADDRESS_16) | UPDI_DATA_8),
        (uint8_t)address,
        (uint8_t)(address >> 8),
        (uint8_t)(address >> 16),
    };
    size_t frame_len = g_address_bits == 24 ? 5 : 4;
    uint8_t ack;
    updi_status_t status = updi_send_receive(frame, frame_len, &ack, 1);
    if (status != UPDI_OK) return status;
    if (ack != UPDI_PHY_ACK) return UPDI_ACK_ERROR;
    status = updi_send_receive(&value, 1, &ack, 1);
    if (status != UPDI_OK) return status;
    return ack == UPDI_PHY_ACK ? UPDI_OK : UPDI_ACK_ERROR;
}

static updi_status_t updi_st16(uint32_t address, const uint8_t data[2]) {
    uint8_t frame[5] = {
        UPDI_PHY_SYNC,
        (uint8_t)(UPDI_STS | (g_address_bits == 24 ? UPDI_ADDRESS_24 : UPDI_ADDRESS_16) |
                  UPDI_DATA_16),
        (uint8_t)address,
        (uint8_t)(address >> 8),
        (uint8_t)(address >> 16),
    };
    uint8_t ack;
    updi_status_t status =
        updi_send_receive(frame, g_address_bits == 24 ? 5 : 4, &ack, 1);
    if (status != UPDI_OK) return status;
    if (ack != UPDI_PHY_ACK) return UPDI_ACK_ERROR;
    status = updi_send_receive(data, 2, &ack, 1);
    if (status != UPDI_OK) return status;
    return ack == UPDI_PHY_ACK ? UPDI_OK : UPDI_ACK_ERROR;
}

static updi_status_t updi_ld(uint32_t address, uint8_t *out) {
    uint8_t frame[5] = {
        UPDI_PHY_SYNC,
        (uint8_t)(UPDI_LDS | (g_address_bits == 24 ? UPDI_ADDRESS_24 : UPDI_ADDRESS_16) | UPDI_DATA_8),
        (uint8_t)address,
        (uint8_t)(address >> 8),
        (uint8_t)(address >> 16),
    };
    return updi_send_receive(frame, g_address_bits == 24 ? 5 : 4, out, 1);
}

static updi_status_t updi_st_ptr(uint32_t address) {
    uint8_t frame[5] = {
        UPDI_PHY_SYNC,
        (uint8_t)(UPDI_ST | UPDI_PTR_ADDRESS |
                  (g_address_bits == 24 ? UPDI_DATA_24 : UPDI_DATA_16)),
        (uint8_t)address,
        (uint8_t)(address >> 8),
        (uint8_t)(address >> 16),
    };
    uint8_t ack;
    updi_status_t status = updi_send_receive(frame, g_address_bits == 24 ? 5 : 4, &ack, 1);
    if (status != UPDI_OK) return status;
    return ack == UPDI_PHY_ACK ? UPDI_OK : UPDI_ACK_ERROR;
}

static updi_status_t updi_repeat_words(uint16_t repeats) {
    if (repeats == 0 || repeats > UPDI_MAX_REPEAT_SIZE + 1) return UPDI_PROTOCOL_ERROR;
    uint8_t frame[3] = {UPDI_PHY_SYNC, UPDI_REPEAT | UPDI_REPEAT_BYTE, (uint8_t)(repeats - 1)};
    return updi_send(frame, 3);
}

static updi_status_t updi_ld_ptr_inc(uint8_t *data, size_t len) {
    uint8_t frame[2] = {UPDI_PHY_SYNC, UPDI_LD | UPDI_PTR_INC | UPDI_DATA_8};
    return updi_send_receive(frame, 2, data, len);
}

static updi_status_t updi_st_ptr_inc16(const uint8_t *data, uint16_t words) {
    uint8_t ctrla_ack_on = 1 << UPDI_CTRLA_IBDLY_BIT;
    uint8_t ctrla_ack_off = ctrla_ack_on | (1 << UPDI_CTRLA_RSD_BIT);
    updi_status_t status = updi_stcs(UPDI_CS_CTRLA, ctrla_ack_off);
    if (status == UPDI_OK) {
        uint8_t frame[2] = {UPDI_PHY_SYNC, UPDI_ST | UPDI_PTR_INC | UPDI_DATA_16};
        status = updi_send(frame, 2);
        if (status == UPDI_OK) {
            status = updi_send(data, (size_t)words * 2);
        }
    }
    updi_status_t restore = updi_stcs(UPDI_CS_CTRLA, ctrla_ack_on);
    return status != UPDI_OK ? status : restore;
}

static updi_status_t updi_write_data_words(uint32_t address, const uint8_t *data, uint16_t words) {
    if (words == 0) return UPDI_OK;
    if (words == 1) return updi_st16(address, data);
    updi_status_t status = updi_st_ptr(address);
    if (status != UPDI_OK) return status;
    status = updi_repeat_words(words);
    if (status != UPDI_OK) return status;
    return updi_st_ptr_inc16(data, words);
}

static updi_status_t updi_read_data(uint32_t address, uint8_t *data, size_t len) {
    while (len > 0) {
        size_t chunk = len > (size_t)UPDI_MAX_REPEAT_SIZE + 1
                           ? (size_t)UPDI_MAX_REPEAT_SIZE + 1
                           : len;
        updi_status_t status = updi_st_ptr(address);
        if (status != UPDI_OK) return status;
        if (chunk > 1) {
            uint8_t frame[3] = {UPDI_PHY_SYNC, UPDI_REPEAT, (uint8_t)(chunk - 1)};
            status = updi_send(frame, 3);
            if (status != UPDI_OK) return status;
        }
        status = updi_ld_ptr_inc(data, chunk);
        if (status != UPDI_OK) return status;
        address += (uint32_t)chunk;
        data += chunk;
        len -= chunk;
    }
    return UPDI_OK;
}

static updi_status_t updi_read_data_direct(uint32_t address, uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        updi_status_t status = updi_ld(address + (uint32_t)i, &data[i]);
        if (status != UPDI_OK) return status;
    }
    return UPDI_OK;
}

static updi_status_t updi_write_key(const uint8_t key[8]) {
    uint8_t reversed[8];
    for (int i = 0; i < 8; i++) reversed[i] = key[7 - i];
    uint8_t frame[2] = {UPDI_PHY_SYNC, UPDI_KEY | UPDI_KEY_KEY | UPDI_KEY_64};
    updi_status_t status = updi_send(frame, 2);
    if (status != UPDI_OK) return status;
    return updi_send(reversed, 8);
}

static updi_status_t updi_read_sib(uint8_t *nvm_version) {
    uint8_t sib[32];
    uint8_t frame[2] = {UPDI_PHY_SYNC, UPDI_KEY | UPDI_KEY_SIB | UPDI_SIB_32BYTES};
    updi_status_t status = updi_send_receive(frame, 2, sib, sizeof(sib));
    if (status != UPDI_OK) return status;

    if (sib[8] != 'P' || sib[9] != ':' ||
        (sib[10] != '0' && sib[10] != '2' && sib[10] != '3' &&
         sib[10] != '4' && sib[10] != '5')) {
        trace("connect: unsupported SIB NVM version");
        return UPDI_UNSUPPORTED_DEVICE;
    }

    *nvm_version = (uint8_t)(sib[10] - '0');
    if (g_trace_fn) {
        char buf[64];
        snprintf(buf, sizeof(buf), "connect: SIB family='%.7s' NVM=P:%u",
                 (const char *)sib, *nvm_version);
        g_trace_fn(buf);
    }
    return UPDI_OK;
}

static updi_status_t updi_init_session_parameters(void) {
    updi_status_t status = updi_stcs(UPDI_CS_CTRLB, 1 << UPDI_CTRLB_CCDETDIS_BIT);
    if (status != UPDI_OK) return status;
    return updi_stcs(UPDI_CS_CTRLA, 1 << UPDI_CTRLA_IBDLY_BIT);
}

static updi_status_t updi_check_datalink(void) {
    uint8_t statusa;
    updi_status_t status = updi_ldcs(UPDI_CS_STATUSA, &statusa);
    if (status != UPDI_OK) {
        trace_status("connect: STATUSA read failed", status);
        return status;
    }
    if (g_trace_fn) {
        char buf[48];
        snprintf(buf, sizeof(buf), "connect: STATUSA=0x%02X", statusa);
        g_trace_fn(buf);
    }
    return statusa != 0 ? UPDI_OK : UPDI_PROTOCOL_ERROR;
}

static updi_status_t updi_apply_reset(bool apply) {
    return updi_stcs(UPDI_ASI_RESET_REQ, apply ? UPDI_RESET_REQ_VALUE : 0);
}

static bool updi_in_progmode(void) {
    uint8_t value;
    if (updi_ldcs(UPDI_ASI_SYS_STATUS, &value) != UPDI_OK) return false;
    return (value & (1 << UPDI_ASI_SYS_STATUS_NVMPROG)) != 0;
}

static updi_status_t updi_wait_unlocked(void) {
    for (int i = 0; i < 10; i++) {
        uint8_t value;
        updi_status_t status = updi_ldcs(UPDI_ASI_SYS_STATUS, &value);
        if (status != UPDI_OK) return status;
        if ((value & (1 << UPDI_ASI_SYS_STATUS_LOCKSTATUS)) == 0) return UPDI_OK;
        g_phy->delay_ms(10);
    }
    return UPDI_LOCKED;
}

static updi_status_t updi_init_and_check(bool send_break) {
    g_phy->set_baud(UPDI_INIT_BAUD, 2);
    if (send_break) {
        uint8_t break_byte = 0x00;
        updi_status_t break_status = updi_send(&break_byte, 1);
        trace_status("connect: break byte send", break_status);
        g_phy->delay_ms(3);
    }
    updi_status_t status = updi_init_session_parameters();
    if (status != UPDI_OK) return status;
    g_phy->set_baud(UPDI_UART_BAUD, 2);
    status = updi_check_datalink();
    if (status == UPDI_OK) return status;
    g_phy->set_baud(UPDI_INIT_BAUD, 2);
    return updi_check_datalink();
}

static void updi_double_break(void) {
    g_phy->deinit();
    g_phy->set_baud(300, 1);
    uint8_t zero = 0x00;
    updi_send(&zero, 1);
    g_phy->delay_ms(100);
    updi_send(&zero, 1);
    g_phy->delay_ms(20);
    g_phy->deinit();
    g_phy->set_baud(UPDI_UART_BAUD, 2);
}

static updi_status_t updi_connect(void) {
    trace("connect: try break/init/check");
    updi_status_t status = updi_init_and_check(true);
    if (status == UPDI_OK) {
        trace("connect: ok first try");
        return UPDI_OK;
    }
    trace_status("connect: first try failed", status);

    trace("connect: double break recovery");
    updi_double_break();
    status = updi_init_and_check(false);
    if (status == UPDI_OK) {
        trace("connect: ok after double break");
        return UPDI_OK;
    }
    trace_status("connect: double break failed", status);

    trace("connect: final retry");
    status = updi_init_and_check(true);
    trace_status("connect: final retry result", status);
    return status;
}

static updi_status_t updi_disconnect(void) {
    updi_status_t reset = updi_apply_reset(true);
    updi_status_t release = updi_apply_reset(false);
    g_phy->delay_ms(5);
    g_phy->flush_rx();
    g_phy->deinit();
    return reset != UPDI_OK ? reset : release;
}

static updi_status_t updi_enter_progmode(void) {
    trace("progmode: check");
    if (updi_in_progmode()) {
        trace("progmode: already in");
        return UPDI_OK;
    }
    trace("progmode: reset apply");
    updi_status_t status = updi_apply_reset(true);
    if (status != UPDI_OK) {
        trace_status("progmode: reset apply failed", status);
        return status;
    }
    trace("progmode: write key");
    status = updi_write_key((const uint8_t *)"NVMProg ");
    if (status != UPDI_OK) {
        trace_status("progmode: write key failed", status);
        return status;
    }
    uint8_t key_status;
    status = updi_ldcs(UPDI_ASI_KEY_STATUS, &key_status);
    if (status != UPDI_OK) {
        trace_status("progmode: read key status failed", status);
        return status;
    }
    if (g_trace_fn) {
        char buf[48];
        snprintf(buf, sizeof(buf), "progmode: key status=0x%02X", key_status);
        g_trace_fn(buf);
    }
    if ((key_status & (1 << UPDI_ASI_KEY_STATUS_NVMPROG)) == 0) {
        trace("progmode: key not accepted");
        return UPDI_PROTOCOL_ERROR;
    }
    trace("progmode: reset cycle");
    status = updi_apply_reset(true);
    if (status != UPDI_OK) return status;
    status = updi_apply_reset(false);
    if (status != UPDI_OK) return status;
    trace("progmode: wait unlocked");
    status = updi_wait_unlocked();
    if (status != UPDI_OK) {
        trace_status("progmode: wait unlocked failed", status);
        return status;
    }
    bool ok = updi_in_progmode();
    trace(ok ? "progmode: ok" : "progmode: still not in progmode after unlock");
    return ok ? UPDI_OK : UPDI_PROTOCOL_ERROR;
}

static updi_status_t updi_start_session(const updi_device_t *device) {
    if (!device || (device->address_bits != 16 && device->address_bits != 24)) {
        return UPDI_UNSUPPORTED_DEVICE;
    }

    g_device = device;
    g_address_bits = device->address_bits;
    g_nvm_version = 0xFF;

    updi_status_t status = updi_connect();
    if (status != UPDI_OK) return status;

    status = updi_read_sib(&g_nvm_version);
    if (status != UPDI_OK) return status;
    if ((g_nvm_version == 0 && g_address_bits != 16) ||
        (g_nvm_version != 0 && g_address_bits != 24)) {
        trace("connect: device metadata/SIB address width mismatch");
        return UPDI_UNSUPPORTED_DEVICE;
    }

    return updi_enter_progmode();
}

static uint16_t updi_flash_write_size(const updi_device_t *device) {
    return device->flash_write_size_override ? device->flash_write_size_override
                                             : device->page_size;
}

static updi_status_t updi_read_device_id_active(const updi_device_t *device,
                                                 updi_target_info_t *target) {
    if (!device || !target) return UPDI_PROTOCOL_ERROR;
    memset(target, 0, sizeof(*target));
    target->name = device->name;
    target->flash_start = device->flash_start;
    target->flash_size = device->flash_size;
    target->page_size = device->page_size;
    target->flash_write_size = updi_flash_write_size(device);
    target->nvm_version = g_nvm_version;

    trace("ping: read signature");
    updi_status_t status = updi_read_data(device->signature_address, target->signature, 3);
    if (status != UPDI_OK) {
        trace_status("ping: signature ptr read failed", status);
        trace("ping: direct signature fallback");
        status = updi_read_data_direct(device->signature_address, target->signature, 3);
        if (status != UPDI_OK) {
            trace_status("ping: direct signature read failed", status);
            return status;
        }
    }
    if (g_trace_fn) {
        char buf[48];
        snprintf(buf, sizeof(buf), "ping: sig=%02X %02X %02X",
                 target->signature[0], target->signature[1], target->signature[2]);
        g_trace_fn(buf);
    }
    uint8_t revision;
    target->revision = (updi_ld((uint32_t)device->syscfg_address + 1, &revision) == UPDI_OK)
                           ? revision
                           : 0;

    if (memcmp(target->signature, device->signature, sizeof(device->signature)) != 0) {
        trace("ping: signature does not match selected device");
        return UPDI_WRONG_DEVICE;
    }
    return UPDI_OK;
}

typedef struct {
    uint8_t status_offset;
    uint8_t error_mask;
    uint8_t busy_mask;
    uint8_t chip_erase_command;
    uint8_t flash_write_command;
    uint8_t page_buffer_clear_command;
    bool page_buffered;
    bool clear_command;
} nvm_profile_t;

static const nvm_profile_t NVM_P0 = {0x02, 0x04, 0x03, 0x05, 0x01, 0x04, true, false};
static const nvm_profile_t NVM_P2 = {0x02, 0x30, 0x03, 0x20, 0x02, 0x00, false, true};
static const nvm_profile_t NVM_P3 = {0x06, 0x70, 0x03, 0x20, 0x04, 0x0F, true, true};
static const nvm_profile_t NVM_P4 = {0x06, 0x70, 0x03, 0x20, 0x02, 0x00, false, true};

static const nvm_profile_t *updi_nvm_profile(void) {
    switch (g_nvm_version) {
        case 0: return &NVM_P0;
        case 2: return &NVM_P2;
        case 3: return &NVM_P3;
        case 4: return &NVM_P4;
        case 5: return &NVM_P3;
        default: return NULL;
    }
}

static updi_status_t updi_execute_nvm_command(uint8_t command) {
    return updi_st(g_device->nvmctrl_address, command);
}

static updi_status_t updi_wait_flash_ready(void) {
    const nvm_profile_t *profile = updi_nvm_profile();
    if (!profile) return UPDI_UNSUPPORTED_DEVICE;

    for (int i = 0; i < 1000; i++) {
        uint8_t status_reg;
        updi_status_t status =
            updi_ld((uint32_t)g_device->nvmctrl_address + profile->status_offset, &status_reg);
        if (status != UPDI_OK) return status;
        if (status_reg & profile->error_mask) return UPDI_NVM_ERROR;
        if ((status_reg & profile->busy_mask) == 0) return UPDI_OK;
        g_phy->delay_ms(10);
    }
    return UPDI_TIMEOUT;
}

static updi_status_t updi_clear_nvm_command(updi_status_t status) {
    const nvm_profile_t *profile = updi_nvm_profile();
    if (profile && profile->clear_command) {
        updi_status_t clear_status = updi_execute_nvm_command(0x00);
        if (status == UPDI_OK) status = clear_status;
    }
    return status;
}

static updi_status_t updi_chip_erase(void) {
    const nvm_profile_t *profile = updi_nvm_profile();
    if (!profile) return UPDI_UNSUPPORTED_DEVICE;

    updi_status_t status = updi_wait_flash_ready();
    if (status == UPDI_OK) status = updi_execute_nvm_command(profile->chip_erase_command);
    if (status == UPDI_OK) status = updi_wait_flash_ready();
    return updi_clear_nvm_command(status);
}

static updi_status_t updi_write_nvm_page(uint32_t address, const uint8_t *data, size_t len,
                                          bool verify) {
    const nvm_profile_t *profile = updi_nvm_profile();
    if (!profile) return UPDI_UNSUPPORTED_DEVICE;

    static uint8_t page[UPDI_MAX_PAGE_SIZE];
    if (len == 0 || len > sizeof(page)) return UPDI_PROTOCOL_ERROR;
    memset(page, 0xFF, sizeof(page));
    memcpy(page, data, len);
    size_t padded_len = (len & 1) ? len + 1 : len;

    if (profile->page_buffered) {
        updi_status_t status = updi_wait_flash_ready();
        if (status != UPDI_OK) return status;
        status = updi_execute_nvm_command(profile->page_buffer_clear_command);
        if (status != UPDI_OK) return status;
        status = updi_wait_flash_ready();
        if (status != UPDI_OK) return status;
        status = updi_write_data_words(address, page, (uint16_t)(padded_len / 2));
        if (status != UPDI_OK) return status;
        status = updi_execute_nvm_command(profile->flash_write_command);
        if (status != UPDI_OK) return status;
        status = updi_wait_flash_ready();
        status = updi_clear_nvm_command(status);
        if (status != UPDI_OK) return status;
    } else {
        size_t write_size = updi_flash_write_size(g_device);
        if (write_size == 0 || (write_size & 1) || write_size > padded_len ||
            padded_len % write_size != 0) {
            return UPDI_UNSUPPORTED_DEVICE;
        }
        for (size_t offset = 0; offset < padded_len; offset += write_size) {
            updi_status_t status = updi_wait_flash_ready();
            if (status == UPDI_OK) {
                status = updi_execute_nvm_command(profile->flash_write_command);
            }
            if (status == UPDI_OK) {
                status = updi_write_data_words(address + (uint32_t)offset, page + offset,
                                               (uint16_t)(write_size / 2));
            }
            if (status == UPDI_OK) status = updi_wait_flash_ready();
            status = updi_clear_nvm_command(status);
            if (status != UPDI_OK) return status;
        }
    }

    if (verify) {
        static uint8_t readback[UPDI_MAX_PAGE_SIZE];
        updi_status_t status = updi_read_data(address, readback, padded_len);
        if (status != UPDI_OK) return status;
        if (memcmp(readback, page, padded_len) != 0) return UPDI_NVM_ERROR;
    }
    return UPDI_OK;
}

static bool hex_nibble(uint8_t byte, uint8_t *out) {
    if (byte >= '0' && byte <= '9') { *out = byte - '0'; return true; }
    if (byte >= 'a' && byte <= 'f') { *out = byte - 'a' + 10; return true; }
    if (byte >= 'A' && byte <= 'F') { *out = byte - 'A' + 10; return true; }
    return false;
}

static bool hex_byte(const uint8_t *hex, size_t hex_len, size_t *cursor, uint8_t *out) {
    if (*cursor + 1 >= hex_len) return false;
    uint8_t hi, lo;
    if (!hex_nibble(hex[*cursor], &hi) || !hex_nibble(hex[*cursor + 1], &lo)) return false;
    *cursor += 2;
    *out = (uint8_t)((hi << 4) | lo);
    return true;
}

static updi_status_t updi_write_hex_pages(const uint8_t *hex, size_t hex_len,
                                           const updi_target_info_t *target, bool verify) {
    size_t page_size = target->page_size;
    if (page_size == 0 || page_size > UPDI_MAX_PAGE_SIZE) return UPDI_UNSUPPORTED_DEVICE;

    static uint8_t page[UPDI_MAX_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    uint32_t page_base = 0xFFFFFFFFu;
    uint32_t extended_base = 0;
    size_t cursor = 0;
    bool wrote_flash = false;

    while (cursor < hex_len) {
        while (cursor < hex_len &&
               (hex[cursor] == '\r' || hex[cursor] == '\n' || hex[cursor] == ' ' || hex[cursor] == '\t')) {
            cursor++;
        }
        if (cursor >= hex_len) break;
        if (hex[cursor] != ':') return UPDI_HEX_ERROR;
        cursor++;

        uint8_t len, addr_hi, addr_lo, record_type;
        if (!hex_byte(hex, hex_len, &cursor, &len)) return UPDI_HEX_ERROR;
        if (!hex_byte(hex, hex_len, &cursor, &addr_hi)) return UPDI_HEX_ERROR;
        if (!hex_byte(hex, hex_len, &cursor, &addr_lo)) return UPDI_HEX_ERROR;
        if (!hex_byte(hex, hex_len, &cursor, &record_type)) return UPDI_HEX_ERROR;
        uint8_t checksum = (uint8_t)(len + addr_hi + addr_lo + record_type);

        uint8_t data[256];
        for (int i = 0; i < len; i++) {
            if (!hex_byte(hex, hex_len, &cursor, &data[i])) return UPDI_HEX_ERROR;
            checksum = (uint8_t)(checksum + data[i]);
        }
        uint8_t file_checksum;
        if (!hex_byte(hex, hex_len, &cursor, &file_checksum)) return UPDI_HEX_ERROR;
        if ((uint8_t)(checksum + file_checksum) != 0) return UPDI_HEX_ERROR;

        if (record_type == 0x01) {
            break;
        } else if (record_type == 0x04) {
            if (len != 2) return UPDI_HEX_ERROR;
            extended_base = (((uint32_t)data[0] << 8) | data[1]) << 16;
        } else if (record_type == 0x00) {
            uint32_t address = extended_base + (((uint32_t)addr_hi << 8) | addr_lo);
            for (int i = 0; i < len; i++) {
                uint32_t absolute = address + i;
                uint32_t offset;
                if (absolute < target->flash_size) {
                    offset = absolute;
                } else if (absolute >= target->flash_start && absolute < target->flash_start + target->flash_size) {
                    offset = absolute - target->flash_start;
                } else {
                    continue;
                }
                uint32_t current_page = offset - (offset % page_size);
                if (page_base == 0xFFFFFFFFu) page_base = current_page;
                if (current_page != page_base) {
                    updi_status_t status = updi_write_nvm_page(
                        target->flash_start + page_base, page, page_size, verify);
                    if (status != UPDI_OK) return status;
                    memset(page, 0xFF, page_size);
                    page_base = current_page;
                }
                page[offset - page_base] = data[i];
                wrote_flash = true;
            }
        }
    }

    if (!wrote_flash) return UPDI_RANGE_ERROR;
    if (page_base != 0xFFFFFFFFu) {
        return updi_write_nvm_page(target->flash_start + page_base, page, page_size, verify);
    }
    return UPDI_OK;
}

static updi_status_t updi_read_signature_at(uint16_t address, uint8_t sig[3]) {
    updi_status_t status = updi_read_data(address, sig, 3);
    if (status != UPDI_OK) {
        status = updi_read_data_direct(address, sig, 3);
    }
    return status;
}

static const updi_device_t *updi_find_device_by_signature(const uint8_t sig[3],
                                                            uint8_t address_bits) {
    for (size_t i = 0; i < UPDI_DEVICE_COUNT; i++) {
        const updi_device_t *candidate = &UPDI_DEVICES[i];
        if (candidate->address_bits == address_bits &&
            memcmp(candidate->signature, sig, 3) == 0) {
            return candidate;
        }
    }
    return NULL;
}

updi_status_t updi_identify(const updi_phy_t *phy, updi_target_info_t *out,
                            const updi_device_t **matched) {
    if (!phy || !out || !matched) return UPDI_PROTOCOL_ERROR;
    memset(out, 0, sizeof(*out));
    *matched = NULL;
    g_phy = phy;
    g_device = NULL;
    g_nvm_version = 0xFF;

    trace("identify: connect");
    updi_status_t status = updi_connect();
    if (status != UPDI_OK) {
        trace_status("identify: connect failed", status);
        updi_disconnect();
        return status;
    }

    status = updi_read_sib(&g_nvm_version);
    if (status != UPDI_OK) {
        trace_status("identify: SIB read failed", status);
        updi_disconnect();
        return status;
    }
    g_address_bits = g_nvm_version == 0 ? 16 : 24;
    out->nvm_version = g_nvm_version;

    status = updi_enter_progmode();
    if (status != UPDI_OK) {
        trace_status("identify: enter progmode failed", status);
        updi_disconnect();
        return status;
    }

    uint8_t sig[3];
    status = updi_read_signature_at(0x1100, sig);
    if (status != UPDI_OK) {
        trace_status("identify: signature read failed", status);
        updi_disconnect();
        return status;
    }

    const updi_device_t *found = updi_find_device_by_signature(sig, g_address_bits);
    if (!found && g_address_bits == 24) {
        trace("identify: no match at 0x1100, trying 0x1080");
        uint8_t sig_alt[3];
        if (updi_read_signature_at(0x1080, sig_alt) == UPDI_OK) {
            const updi_device_t *found_alt = updi_find_device_by_signature(sig_alt, g_address_bits);
            if (found_alt) {
                found = found_alt;
                memcpy(sig, sig_alt, 3);
            }
        }
    }
    memcpy(out->signature, sig, 3);
    if (g_trace_fn) {
        char buf[64];
        snprintf(buf, sizeof(buf), "identify: sig=%02X %02X %02X nvm=P:%u addr_bits=%u",
                 sig[0], sig[1], sig[2], g_nvm_version, g_address_bits);
        g_trace_fn(buf);
    }

    uint8_t revision;
    if (updi_ld(0x0F01, &revision) == UPDI_OK) out->revision = revision;

    if (found) {
        out->name = found->name;
        out->flash_start = found->flash_start;
        out->flash_size = found->flash_size;
        out->page_size = found->page_size;
        out->flash_write_size = updi_flash_write_size(found);
    }
    *matched = found;

    updi_disconnect();
    return UPDI_OK;
}

updi_status_t updi_ping(const updi_phy_t *phy, const updi_device_t *device,
                        updi_target_info_t *out) {
    if (!phy || !device || !out) return UPDI_PROTOCOL_ERROR;
    g_phy = phy;
    updi_status_t status = updi_start_session(device);
    if (status == UPDI_OK) status = updi_read_device_id_active(device, out);
    updi_disconnect();
    return status;
}

updi_status_t updi_erase(const updi_phy_t *phy, const updi_device_t *device) {
    if (!phy || !device) return UPDI_PROTOCOL_ERROR;
    g_phy = phy;

    trace("erase: connect");
    updi_status_t status = updi_start_session(device);
    if (status != UPDI_OK) {
        trace_status("erase: session failed", status);
        updi_disconnect();
        return status;
    }

    trace("erase: verify device");
    updi_target_info_t target;
    status = updi_read_device_id_active(device, &target);
    if (status == UPDI_OK) {
        trace("erase: command");
        status = updi_chip_erase();
    }
    trace_status("erase: result before disconnect", status);
    updi_disconnect();
    return status;
}

updi_status_t updi_flash_hex(const updi_phy_t *phy, const updi_device_t *device,
                             const uint8_t *hex, size_t hex_len,
                             bool erase_first, bool verify) {
    if (!phy || !device || !hex) return UPDI_PROTOCOL_ERROR;
    if (hex_len == 0) return UPDI_HEX_ERROR;
    g_phy = phy;

    trace("flash: connect");
    updi_status_t status = updi_start_session(device);
    if (status != UPDI_OK) {
        trace_status("flash: session failed", status);
        updi_disconnect();
        return status;
    }

    trace("flash: read device");
    updi_target_info_t target = {0};
    status = updi_read_device_id_active(device, &target);
    if (status != UPDI_OK) {
        trace_status("flash: read device failed", status);
        updi_disconnect();
        return status;
    }
    if (g_trace_fn) {
        char buf[80];
        snprintf(buf, sizeof(buf), "flash: detected %s flash=%lu page=%u",
                 target.name, (unsigned long)target.flash_size, target.page_size);
        g_trace_fn(buf);
    }

    if (erase_first) {
        trace("flash: erase command");
        status = updi_chip_erase();
        if (status == UPDI_OK) {
            trace("flash: wait unlocked");
            status = updi_wait_unlocked();
        }
        if (status == UPDI_OK) {
            trace("flash: reenter progmode");
            status = updi_enter_progmode();
        }
        if (status != UPDI_OK) {
            trace_status("flash: erase-first failed", status);
            updi_disconnect();
            return status;
        }
    }

    trace("flash: write pages");
    status = updi_write_hex_pages(hex, hex_len, &target, verify);
    trace_status("flash: result before disconnect", status);
    updi_disconnect();
    return status;
}

size_t updi_trim_hex_len(const uint8_t *data, size_t len) {
    while (len > 0 && data[len - 1] == 0) len--;
    return len;
}

