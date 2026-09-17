#include "badge_updiprog.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "badge_ansi.h"
#include "badge_files.h"
#include "badge_hardware.h"
#include "updi.h"
#include "updi_uart.h"

enum {
    UPDI_HEX_BUFFER_SIZE = 64 * 1024,
};

static uint8_t hex_buffer[UPDI_HEX_BUFFER_SIZE];

static void print_prompt(void) {
    printf(BADGE_ANSI_RESET BADGE_ANSI_BOLD BADGE_ANSI_BLUE "%s"
           BADGE_ANSI_RESET, badge_updiprog_prompt());
    fflush(stdout);
}

static void print_menu(void) {
    printf("\n" BADGE_ANSI_BOLD BADGE_ANSI_MAGENTA
           "UPDI programmer - UART0 GPIO16 TX / GPIO17 RX"
           BADGE_ANSI_RESET "\n" BADGE_ANSI_LAVENDER);
    puts("\nProgram and reprogram your SAO, which uses a UPDI-based chip\n"
         "(ATtiny412). UPDI uses a one-wire, UART-based protocol for\n"
         "flashing, erasing, and identifying supported chips. The\n"
         "debugging protocol is proprietary, so this feature does not\n"
         "support debugging.\n");
    puts("To use this programmer with ANY other supported chip, add its\n"
         "firmware HEX file (compiled with MPLAB, Arduino, or PlatformIO)\n"
         "to the mass-storage filesystem. Then use 'ping' to identify the\n"
         "chip and pass its name to the 'erase' or 'flash' command.\n");
    printf("Supported targets: %u (use 'devices' to list)\n",
           (unsigned)UPDI_DEVICE_COUNT);
    puts("Commands:");
    puts("  ping                         auto-detect the connected target");
    puts("  erase <device>               chip-erase the named target");
    puts("  flash <device> <file.hex>    erase, program, and verify");
    puts("  devices                      list supported UPDI targets");
    puts("  files                        list files available to flash");
    puts("  back");
    print_prompt();
}

static bool equal_ignore_case(const char *left, const char *right) {
    while (*left && *right) {
        if (tolower((unsigned char)*left) !=
            tolower((unsigned char)*right)) {
            return false;
        }
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static const updi_device_t *find_device(const char *name) {
    for (size_t i = 0; i < UPDI_DEVICE_COUNT; ++i) {
        if (equal_ignore_case(name, UPDI_DEVICES[i].name)) {
            return &UPDI_DEVICES[i];
        }
    }
    return NULL;
}

static char *trim(char *text) {
    while (isspace((unsigned char)*text)) ++text;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}

static bool filename_is_hex(const char *name) {
    size_t length = strlen(name);
    return length > 4 && equal_ignore_case(name + length - 4, ".hex");
}

static updi_status_t detect_target(updi_target_info_t *target,
                                   const updi_device_t **matched) {
    updi_status_t status = updi_identify(&UPDI_UART_PHY, target, matched);
    if (status != UPDI_OK) {
        printf("Ping FAILED: %s\n", updi_status_str(status));
    } else if (!*matched) {
        printf("Ping FAILED: unsupported ID %02X %02X %02X, NVM P:%u\n",
               target->signature[0], target->signature[1],
               target->signature[2], target->nvm_version);
        status = UPDI_UNSUPPORTED_DEVICE;
    } else {
        printf("Ping SUCCESS: %s, ID %02X %02X %02X, revision %02X, NVM P:%u\n",
               (*matched)->name, target->signature[0], target->signature[1],
               target->signature[2], target->revision, target->nvm_version);
    }
    return status;
}

static void print_devices(void) {
    for (size_t i = 0; i < UPDI_DEVICE_COUNT; ++i) {
        const updi_device_t *device = &UPDI_DEVICES[i];
        printf("  %-12s %02X %02X %02X  %lu bytes\n", device->name,
               device->signature[0], device->signature[1],
               device->signature[2], (unsigned long)device->flash_size);
    }
}

static void print_files(void) {
    badge_files_status_t status = badge_files_prepare();
    if (status == BADGE_FILES_ERROR) {
        puts("File storage is unavailable.");
    } else if (status == BADGE_FILES_UNSUPPORTED) {
        puts("File storage is not FAT12/FAT16.");
    } else {
        if (status == BADGE_FILES_REPAIRED) {
            puts("Repaired the filesystem capacity.");
        }
        badge_files_print_listing();
    }
}

static void erase_target(char *argument) {
    char *name = trim(argument);
    const updi_device_t *device = find_device(name);
    if (!device) {
        puts("Unknown device. Use 'devices' to list valid names.");
        print_prompt();
        return;
    }

    updi_status_t status = updi_erase(&UPDI_UART_PHY, device);
    printf("Erase %s: %s, ID %02X %02X %02X\n",
           status == UPDI_OK ? "SUCCESS" : "FAILED", device->name,
           device->signature[0], device->signature[1], device->signature[2]);
    if (status != UPDI_OK) printf("Reason: %s\n", updi_status_str(status));
    print_prompt();
}

static void flash_file(char *argument) {
    char *text = trim(argument);
    char *separator = strpbrk(text, " \t");
    if (!separator) {
        puts("Use: flash <device> <root-file.hex>");
        print_prompt();
        return;
    }
    *separator = '\0';
    const updi_device_t *device = find_device(text);
    char *name = trim(separator + 1);

    if (!device) {
        puts("Unknown device. Use 'devices' to list valid names.");
        print_prompt();
        return;
    }

    if (!filename_is_hex(name)) {
        puts("Use: flash <device> <root-file.hex>");
        print_prompt();
        return;
    }

    badge_files_status_t files = badge_files_prepare();
    size_t hex_length = 0;
    if (files == BADGE_FILES_ERROR || files == BADGE_FILES_UNSUPPORTED ||
        !badge_files_read_file(name, (char *)hex_buffer, sizeof(hex_buffer),
                              &hex_length)) {
        puts("HEX file not found, invalid, or too large (64 KiB maximum).");
        print_prompt();
        return;
    }
    hex_length = updi_trim_hex_len(hex_buffer, hex_length);

    updi_status_t status = updi_flash_hex(&UPDI_UART_PHY, device, hex_buffer,
                                          hex_length, true, true);
    printf("Flash %s: %s -> %s, ID %02X %02X %02X, %lu-byte HEX%s\n",
           status == UPDI_OK ? "SUCCESS" : "FAILED", name, device->name,
           device->signature[0], device->signature[1], device->signature[2],
           (unsigned long)hex_length,
           status == UPDI_OK ? ", erase/program/verify OK" : "");
    if (status != UPDI_OK) printf("Reason: %s\n", updi_status_str(status));
    print_prompt();
}

void badge_updiprog_enter(void) {
    badge_gpio12_uart_stop();
    updi_uart_init();
    updi_set_trace(NULL);
    print_menu();
}

void badge_updiprog_leave(void) {
    updi_uart_deinit();
    badge_gpio12_uart_stop();
}

const char *badge_updiprog_prompt(void) {
    return "updi> ";
}

bool badge_updiprog_handle_command(char *command) {
    if (!strcmp(command, "1") || !strcmp(command, "ping")) {
        updi_target_info_t target = {0};
        const updi_device_t *matched = NULL;
        (void)detect_target(&target, &matched);
        print_prompt();
    } else if (!strncmp(command, "erase ", 6)) {
        erase_target(command + 6);
    } else if (!strcmp(command, "2") || !strcmp(command, "erase")) {
        puts("Use: erase <device>");
        print_prompt();
    } else if (!strncmp(command, "flash ", 6)) {
        flash_file(command + 6);
    } else if (!strcmp(command, "3") || !strcmp(command, "flash")) {
        puts("Use: flash <device> <root-file.hex>");
        print_prompt();
    } else if (!strcmp(command, "devices")) {
        print_devices();
        print_prompt();
    } else if (!strcmp(command, "files")) {
        print_files();
        print_prompt();
    } else if (!strcmp(command, "4") || !strcmp(command, "back")) {
        badge_updiprog_leave();
        return false;
    } else {
        puts("Choose 'ping', 'erase <device>', 'flash <device> <file.hex>', 'devices', 'files', or 'back'.");
        print_prompt();
    }
    return true;
}
