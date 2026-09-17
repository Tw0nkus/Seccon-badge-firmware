#include "badge_ctf.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "badge_hardware.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "pico/bootrom.h"
#include "pico/flash.h"
#include "pico/sha256.h"
#include "pico/stdlib.h"

#define OTP_ROOT_KEY_FIRST_ROW 0x0c0u
#define OTP_ROOT_KEY_SIZE 32u
#define OTP_ROOT_KEY_ROW_COUNT (OTP_ROOT_KEY_SIZE / sizeof(uint16_t))

typedef struct {
    const char *context;
    const char *prefix;
} flag_definition_t;

enum {
    FLAG_TOKEN_BYTES = 16,
    PROGRESS_TAG_BYTES = 16,
    PROGRESS_PAGE_WELCOME = 0,
    PROGRESS_PAGE_GPIO_LEVEL_1,
    PROGRESS_PAGE_GPIO_LEVEL_2,
    PROGRESS_PAGE_GPIO_LEVEL_3,
    PROGRESS_PAGE_GPIO_LEVEL_4,
    PROGRESS_PAGE_GPIO2_SOLVED,
    PROGRESS_PAGE_GPIO1_SOLVED,
    PROGRESS_PAGE_GPIO3_SOLVED,
    PROGRESS_PAGE_NEOPIXEL1_SOLVED,
    PROGRESS_PAGE_NEOPIXEL2_SOLVED,
    PROGRESS_PAGE_UART1_SOLVED,
    PROGRESS_PAGE_UART2_QUIZ_SOLVED_LEGACY,
    PROGRESS_PAGE_UART2_SOLVED,
    PROGRESS_PAGE_I2C1_SOLVED,
    PROGRESS_PAGE_I2C2_SOLVED,
    PROGRESS_PAGE_GERBER_SOLVED,
    PROGRESS_PAGE_SCHEMATIC_SOLVED,
    PROGRESS_PAGE_COUNT,
};

static const flag_definition_t flags[] = {
    [BADGE_FLAG_WELCOME] = {"badge/flag/v1/welcome", "FLAG{welcome_"},
    [BADGE_FLAG_GPIO1] = {"badge/flag/v1/gpio1", "FLAG{GPIO1_"},
    [BADGE_FLAG_GPIO2] = {"badge/flag/v1/gpio2", "FLAG{GPIO2_"},
    [BADGE_FLAG_GPIO3] = {"badge/flag/v1/gpio3", "FLAG{GPIO3_"},
    [BADGE_FLAG_UART1] = {"badge/flag/v1/uart1", "FLAG{UART1_"},
    [BADGE_FLAG_UART2] = {"badge/flag/v1/uart2", "FLAG{UART2_"},
};
static const char *const progress_contexts[] = {
    [PROGRESS_PAGE_WELCOME] = "badge/progress/v1/welcome",
    [PROGRESS_PAGE_GPIO_LEVEL_1] = "badge/progress/v1/gpio2/level1",
    [PROGRESS_PAGE_GPIO_LEVEL_2] = "badge/progress/v1/gpio2/level2",
    [PROGRESS_PAGE_GPIO_LEVEL_3] = "badge/progress/v1/gpio2/level3",
    [PROGRESS_PAGE_GPIO_LEVEL_4] = "badge/progress/v1/gpio2/level4",
    [PROGRESS_PAGE_GPIO2_SOLVED] = "badge/progress/v1/gpio2/solved",
    [PROGRESS_PAGE_GPIO1_SOLVED] = "badge/progress/v1/gpio1/solved",
    [PROGRESS_PAGE_GPIO3_SOLVED] = "badge/progress/v1/gpio3/solved",
    [PROGRESS_PAGE_NEOPIXEL1_SOLVED] =
        "badge/progress/v1/neopixel1/solved",
    [PROGRESS_PAGE_NEOPIXEL2_SOLVED] =
        "badge/progress/v1/neopixel2/solved",
    [PROGRESS_PAGE_UART1_SOLVED] = "badge/progress/v1/uart1/solved",
    [PROGRESS_PAGE_UART2_QUIZ_SOLVED_LEGACY] =
        "badge/progress/v1/uart2/solved",
    [PROGRESS_PAGE_UART2_SOLVED] =
        "badge/progress/v2/uart2/flag-submitted",
    [PROGRESS_PAGE_I2C1_SOLVED] = "badge/progress/v1/i2c1/solved",
    [PROGRESS_PAGE_I2C2_SOLVED] = "badge/progress/v1/i2c2/solved",
    [PROGRESS_PAGE_GERBER_SOLVED] =
        "badge/progress/v1/hardware-files/gerber/solved",
    [PROGRESS_PAGE_SCHEMATIC_SOLVED] =
        "badge/progress/v1/hardware-files/schematic/solved",
};
static const uint8_t gerber_flag_digest[SHA256_RESULT_BYTES] = {
    0x0c, 0xf0, 0xc1, 0x53, 0x99, 0x34, 0xf1, 0x16,
    0x7f, 0xb4, 0x60, 0x5d, 0xb7, 0xc3, 0xfb, 0x70,
    0x4f, 0xb5, 0xd1, 0x12, 0xb9, 0x22, 0xd4, 0xd6,
    0xa0, 0x7e, 0x27, 0x71, 0x7c, 0xf2, 0x12, 0x36,
};
static const uint8_t i2c1_flag_digest[SHA256_RESULT_BYTES] = {
    0x15, 0xfe, 0x20, 0x8c, 0x0f, 0xf1, 0x48, 0x61,
    0x95, 0xe1, 0x25, 0x24, 0x1f, 0x74, 0x0c, 0xc4,
    0x73, 0xe0, 0xb3, 0x3b, 0xab, 0x2b, 0x5a, 0x7a,
    0x7e, 0xe1, 0x83, 0x69, 0x82, 0x47, 0xa4, 0x80,
};
static const uint8_t i2c2_flag_digest[SHA256_RESULT_BYTES] = {
    0x80, 0x87, 0x59, 0xa0, 0xd7, 0x06, 0x24, 0x2a,
    0x97, 0x80, 0xbe, 0x69, 0xc1, 0x2b, 0xe3, 0x95,
    0x00, 0x1f, 0x17, 0xd9, 0x97, 0x1f, 0x7f, 0xa3,
    0xce, 0x6b, 0xf0, 0xf3, 0xc0, 0xe3, 0x9a, 0xce,
};
static const uint8_t schematic_flag_digest[SHA256_RESULT_BYTES] = {
    0x90, 0xb0, 0xc1, 0x9e, 0xfc, 0x75, 0x4a, 0xcb,
    0xa2, 0x51, 0x4d, 0x7b, 0xab, 0xe7, 0xe9, 0x88,
    0x3a, 0x40, 0x1d, 0x6d, 0x43, 0x8c, 0xc6, 0x56,
    0xca, 0x2f, 0xfc, 0xa5, 0x97, 0xf9, 0x1c, 0xa1,
};
static const uint8_t progress_magic[] = "CTFSOLV1";
static const uint8_t uart_quiz_magic[] = "UARTQZ1";
static const uint8_t uart_quiz_key_context[] = "uart-quiz-v1:key";
static const uint8_t uart_quiz_answer_context[] = "uart-quiz-v1:answers:";
static const uint8_t uart_quiz_record_context[] = "uart-quiz-v1:record:";
_Static_assert(sizeof(flags) / sizeof(flags[0]) == BADGE_FLAG_COUNT,
               "flag list must match flag enum");
_Static_assert(sizeof(progress_contexts) / sizeof(progress_contexts[0]) ==
                   PROGRESS_PAGE_COUNT,
               "progress contexts must match progress pages");
_Static_assert(PROGRESS_PAGE_COUNT * FLASH_PAGE_SIZE <=
                   2 * FLASH_SECTOR_SIZE,
               "progress pages must fit their flash sectors");

enum {
    PROGRESS_RECORD_BYTES = sizeof(progress_magic) - 1 + PROGRESS_TAG_BYTES,
};

typedef struct {
    uint32_t offset;
    const uint8_t *data;
} progress_write_t;

typedef struct {
    uint8_t magic[sizeof(uart_quiz_magic)];
    uint8_t expected_tag[SHA256_RESULT_BYTES];
    uint8_t authentication_tag[SHA256_RESULT_BYTES];
} uart_quiz_record_t;

extern char __flag_progress_start;
extern char __flag_progress_extension_start;
extern char __uart_quiz_record_start;

static bool welcome_solved;
static bool gpio1_solved;
static bool gpio2_solved;
static bool gpio3_solved;
static bool neopixel1_solved;
static bool neopixel2_solved;
static bool uart1_solved;
static bool uart2_solved;
static bool i2c1_solved;
static bool i2c2_solved;
static bool gerber_solved;
static bool schematic_solved;
static bool uart2_available;
static uint8_t gpio_levels_solved;

static const badge_gpio_state_t gpio_level_requirements
    [BADGE_CTF_GPIO_LEVEL_COUNT][BADGE_GPIO_BRIDGE_COUNT] = {
        {BADGE_GPIO_HIGH, BADGE_GPIO_FLOATING, BADGE_GPIO_FLOATING,
         BADGE_GPIO_FLOATING},
        {BADGE_GPIO_LOW, BADGE_GPIO_FLOATING, BADGE_GPIO_FLOATING,
         BADGE_GPIO_HIGH},
        {BADGE_GPIO_FLOATING, BADGE_GPIO_HIGH, BADGE_GPIO_LOW,
         BADGE_GPIO_LOW},
        {BADGE_GPIO_HIGH, BADGE_GPIO_LOW, BADGE_GPIO_HIGH, BADGE_GPIO_LOW},
};

static void zero_bytes(void *buffer, size_t length) {
    volatile uint8_t *bytes = buffer;
    while (length--) {
        *bytes++ = 0;
    }
}

static int read_otp_root_key(uint8_t root_key[OTP_ROOT_KEY_SIZE]) {
    uint16_t rows[OTP_ROOT_KEY_ROW_COUNT];
    otp_cmd_t command = {
        .flags = OTP_ROOT_KEY_FIRST_ROW | OTP_CMD_ECC_BITS,
    };
    int result = rom_func_otp_access((uint8_t *)rows, sizeof(rows), command);
    if (result == BOOTROM_OK) {
        memcpy(root_key, rows, sizeof(rows));
    }
    zero_bytes(rows, sizeof(rows));
    return result;
}

static bool key_is_blank(const uint8_t key[OTP_ROOT_KEY_SIZE]) {
    uint8_t combined = 0;
    for (size_t i = 0; i < OTP_ROOT_KEY_SIZE; ++i) {
        combined |= key[i];
    }
    return combined == 0;
}

static int hmac_sha256(const uint8_t key[OTP_ROOT_KEY_SIZE],
                       const uint8_t *message, size_t message_length,
                       uint8_t digest[SHA256_RESULT_BYTES]) {
    uint8_t pad[64] = {0};
    sha256_result_t inner_digest;
    sha256_result_t outer_digest;
    pico_sha256_state_t state;

    memcpy(pad, key, OTP_ROOT_KEY_SIZE);
    for (size_t i = 0; i < sizeof(pad); ++i) {
        pad[i] ^= 0x36;
    }

    int result = pico_sha256_start_blocking(&state, SHA256_BIG_ENDIAN, false);
    if (result != PICO_OK) {
        goto done;
    }
    pico_sha256_update_blocking(&state, pad, sizeof(pad));
    pico_sha256_update_blocking(&state, message, message_length);
    pico_sha256_finish(&state, &inner_digest);

    for (size_t i = 0; i < sizeof(pad); ++i) {
        pad[i] ^= 0x36 ^ 0x5c;
    }
    result = pico_sha256_start_blocking(&state, SHA256_BIG_ENDIAN, false);
    if (result != PICO_OK) {
        goto done;
    }
    pico_sha256_update_blocking(&state, pad, sizeof(pad));
    pico_sha256_update_blocking(&state, inner_digest.bytes, sizeof(inner_digest.bytes));
    pico_sha256_finish(&state, &outer_digest);
    memcpy(digest, outer_digest.bytes, sizeof(outer_digest.bytes));

done:
    zero_bytes(pad, sizeof(pad));
    zero_bytes(&inner_digest, sizeof(inner_digest));
    zero_bytes(&outer_digest, sizeof(outer_digest));
    return result;
}

static bool constant_time_equal(const uint8_t *left, const uint8_t *right,
                                size_t length) {
    volatile uint8_t difference = 0;
    for (size_t i = 0; i < length; ++i) {
        difference |= left[i] ^ right[i];
    }
    return difference == 0;
}

static bool flag_digest_matches(
    const char *submitted_flag,
    const uint8_t expected_digest[SHA256_RESULT_BYTES]) {
    pico_sha256_state_t state;
    sha256_result_t submitted_digest = {0};
    bool matches = false;

    if (pico_sha256_start_blocking(&state, SHA256_BIG_ENDIAN, false) ==
        PICO_OK) {
        pico_sha256_update_blocking(&state,
                                    (const uint8_t *)submitted_flag,
                                    strlen(submitted_flag));
        pico_sha256_finish(&state, &submitted_digest);
        matches = constant_time_equal(submitted_digest.bytes, expected_digest,
                                      sizeof(submitted_digest.bytes));
    }
    zero_bytes(&submitted_digest, sizeof(submitted_digest));
    return matches;
}

static int derive_uart_quiz_key(uint8_t quiz_key[SHA256_RESULT_BYTES]) {
    uint8_t root_key[OTP_ROOT_KEY_SIZE] = {0};
    int result = read_otp_root_key(root_key);
    if (result == BOOTROM_OK && key_is_blank(root_key)) {
        result = 1;
    } else if (result == BOOTROM_OK) {
        result = hmac_sha256(root_key, uart_quiz_key_context,
                             sizeof(uart_quiz_key_context) - 1, quiz_key);
    }
    zero_bytes(root_key, sizeof(root_key));
    return result;
}

static const uart_quiz_record_t *uart_quiz_record(void) {
    uint32_t offset =
        (uint32_t)((uintptr_t)&__uart_quiz_record_start - XIP_BASE);
    return (const uart_quiz_record_t *)(
        XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + offset);
}

static bool uart_quiz_record_is_valid(
    const uart_quiz_record_t *record,
    const uint8_t quiz_key[SHA256_RESULT_BYTES]) {
    uint8_t message[sizeof(uart_quiz_record_context) - 1 +
                    sizeof(record->magic) + sizeof(record->expected_tag)];
    uint8_t expected[SHA256_RESULT_BYTES] = {0};
    bool valid = false;

    if (memcmp(record->magic, uart_quiz_magic, sizeof(record->magic))) {
        return false;
    }

    size_t position = 0;
    memcpy(message + position, uart_quiz_record_context,
           sizeof(uart_quiz_record_context) - 1);
    position += sizeof(uart_quiz_record_context) - 1;
    memcpy(message + position, record->magic, sizeof(record->magic));
    position += sizeof(record->magic);
    memcpy(message + position, record->expected_tag,
           sizeof(record->expected_tag));

    if (hmac_sha256(quiz_key, message, sizeof(message), expected) == PICO_OK) {
        valid = constant_time_equal(expected, record->authentication_tag,
                                    sizeof(expected));
    }
    zero_bytes(message, sizeof(message));
    zero_bytes(expected, sizeof(expected));
    return valid;
}

static bool uart_quiz_is_available(void) {
    uint8_t quiz_key[SHA256_RESULT_BYTES] = {0};
    bool available = derive_uart_quiz_key(quiz_key) == PICO_OK &&
                     uart_quiz_record_is_valid(uart_quiz_record(), quiz_key);
    zero_bytes(quiz_key, sizeof(quiz_key));
    return available;
}

static int make_progress_record(uint8_t page, uint8_t record[FLASH_PAGE_SIZE]) {
    uint8_t root_key[OTP_ROOT_KEY_SIZE] = {0};
    uint8_t digest[SHA256_RESULT_BYTES] = {0};
    if (page >= PROGRESS_PAGE_COUNT) {
        return 1;
    }

    int result = read_otp_root_key(root_key);
    memset(record, 0xff, FLASH_PAGE_SIZE);
    if (result == BOOTROM_OK && key_is_blank(root_key)) {
        result = 1;
    } else if (result == BOOTROM_OK) {
        result = hmac_sha256(
            root_key, (const uint8_t *)progress_contexts[page],
            strlen(progress_contexts[page]), digest);
        if (result == PICO_OK) {
            memcpy(record, progress_magic, sizeof(progress_magic) - 1);
            memcpy(record + sizeof(progress_magic) - 1, digest, PROGRESS_TAG_BYTES);
        }
    }

    zero_bytes(root_key, sizeof(root_key));
    zero_bytes(digest, sizeof(digest));
    return result;
}

static uint32_t progress_flash_offset(uint8_t page) {
    const uint8_t pages_per_sector = FLASH_SECTOR_SIZE / FLASH_PAGE_SIZE;
    if (page < pages_per_sector) {
        return (uint32_t)((uintptr_t)&__flag_progress_start - XIP_BASE) +
               page * FLASH_PAGE_SIZE;
    }
    return (uint32_t)((uintptr_t)&__flag_progress_extension_start - XIP_BASE) +
           (page - pages_per_sector) * FLASH_PAGE_SIZE;
}

static const uint8_t *progress_flash(uint8_t page) {
    return (const uint8_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE +
                             progress_flash_offset(page));
}

static bool progress_is_solved(uint8_t page) {
    uint8_t expected[FLASH_PAGE_SIZE];
    bool solved = make_progress_record(page, expected) == PICO_OK &&
                  !memcmp(progress_flash(page), expected, PROGRESS_RECORD_BYTES);
    zero_bytes(expected, sizeof(expected));
    return solved;
}

static void program_progress_page(void *parameter) {
    const progress_write_t *write = parameter;
    flash_range_program(write->offset, write->data, FLASH_PAGE_SIZE);
}

static bool save_progress(uint8_t page) {
    uint8_t record[FLASH_PAGE_SIZE];
    const uint8_t *stored = progress_flash(page);
    bool blank = true;
    bool saved = false;

    if (make_progress_record(page, record) != PICO_OK) {
        goto done;
    }
    if (!memcmp(stored, record, PROGRESS_RECORD_BYTES)) {
        saved = true;
        goto done;
    }
    for (size_t i = 0; i < FLASH_PAGE_SIZE; ++i) {
        blank &= stored[i] == 0xff;
    }
    if (!blank) {
        goto done;
    }

    progress_write_t write = {
        .offset = progress_flash_offset(page),
        .data = record,
    };
    saved = flash_safe_execute(program_progress_page, &write, UINT32_MAX) == PICO_OK &&
            !memcmp(progress_flash(page), record, PROGRESS_RECORD_BYTES);

done:
    zero_bytes(record, sizeof(record));
    return saved;
}

int badge_ctf_make_flag(
    badge_flag_id_t id, char output[BADGE_CTF_FLAG_TEXT_SIZE]) {
    uint8_t root_key[OTP_ROOT_KEY_SIZE] = {0};
    uint8_t digest[SHA256_RESULT_BYTES] = {0};
    if ((unsigned)id >= BADGE_FLAG_COUNT) {
        return 1;
    }
    int result = read_otp_root_key(root_key);

    if (result == BOOTROM_OK && key_is_blank(root_key)) {
        result = 1;
    } else if (result == BOOTROM_OK) {
        result = hmac_sha256(
            root_key, (const uint8_t *)flags[id].context,
            strlen(flags[id].context), digest);
        if (result == PICO_OK) {
            static const char hex[] = "0123456789abcdef";
            size_t position = 0;
            size_t prefix_length = strlen(flags[id].prefix);
            memcpy(output, flags[id].prefix, prefix_length);
            position += prefix_length;
            for (size_t i = 0; i < FLAG_TOKEN_BYTES; ++i) {
                output[position++] = hex[digest[i] >> 4];
                output[position++] = hex[digest[i] & 0x0f];
            }
            output[position++] = '}';
            output[position] = '\0';
        }
    }

    zero_bytes(root_key, sizeof(root_key));
    zero_bytes(digest, sizeof(digest));
    return result;
}

static uint8_t load_gpio_levels_solved(void) {
    uint8_t solved = 0;
    while (solved < BADGE_CTF_GPIO_LEVEL_COUNT &&
           progress_is_solved(PROGRESS_PAGE_GPIO_LEVEL_1 + solved)) {
        ++solved;
    }
    return solved;
}

static bool gpio_level_matches(uint8_t level) {
    badge_gpio_state_t actual[BADGE_GPIO_BRIDGE_COUNT];
    badge_gpio_read_bridges(actual);
    return !memcmp(actual, gpio_level_requirements[level], sizeof(actual));
}

void badge_ctf_init(void) {
    welcome_solved = progress_is_solved(PROGRESS_PAGE_WELCOME);
    gpio1_solved = progress_is_solved(PROGRESS_PAGE_GPIO1_SOLVED);
    gpio3_solved = progress_is_solved(PROGRESS_PAGE_GPIO3_SOLVED);
    neopixel1_solved = progress_is_solved(PROGRESS_PAGE_NEOPIXEL1_SOLVED);
    neopixel2_solved = progress_is_solved(PROGRESS_PAGE_NEOPIXEL2_SOLVED);
    uart1_solved = progress_is_solved(PROGRESS_PAGE_UART1_SOLVED);
    uart2_solved = progress_is_solved(PROGRESS_PAGE_UART2_SOLVED);
    i2c1_solved = progress_is_solved(PROGRESS_PAGE_I2C1_SOLVED);
    i2c2_solved = progress_is_solved(PROGRESS_PAGE_I2C2_SOLVED);
    gerber_solved = progress_is_solved(PROGRESS_PAGE_GERBER_SOLVED);
    schematic_solved = progress_is_solved(PROGRESS_PAGE_SCHEMATIC_SOLVED);
    uart2_available = uart_quiz_is_available();
    gpio_levels_solved = load_gpio_levels_solved();
    if (gpio_levels_solved < BADGE_CTF_GPIO_LEVEL_COUNT &&
        gpio_level_matches(gpio_levels_solved) &&
        save_progress(PROGRESS_PAGE_GPIO_LEVEL_1 + gpio_levels_solved)) {
        ++gpio_levels_solved;
    }
    gpio2_solved = gpio_levels_solved == BADGE_CTF_GPIO_LEVEL_COUNT &&
                   progress_is_solved(PROGRESS_PAGE_GPIO2_SOLVED);
}

badge_uart_quiz_result_t badge_ctf_submit_uart2_answers(
    const char answers[BADGE_CTF_UART_QUIZ_ANSWER_COUNT]) {
    uint8_t quiz_key[SHA256_RESULT_BYTES] = {0};
    uint8_t submitted_tag[SHA256_RESULT_BYTES] = {0};
    uint8_t message[sizeof(uart_quiz_answer_context) - 1 +
                    BADGE_CTF_UART_QUIZ_ANSWER_COUNT] = {0};
    badge_uart_quiz_result_t result = BADGE_UART_QUIZ_UNAVAILABLE;
    const uart_quiz_record_t *record = uart_quiz_record();

    if (!answers || !uart2_available ||
        derive_uart_quiz_key(quiz_key) != PICO_OK ||
        !uart_quiz_record_is_valid(record, quiz_key)) {
        goto done;
    }
    for (size_t i = 0; i < BADGE_CTF_UART_QUIZ_ANSWER_COUNT; ++i) {
        if (answers[i] < 'A' || answers[i] > 'D') {
            result = BADGE_UART_QUIZ_INCORRECT;
            goto done;
        }
    }

    memcpy(message, uart_quiz_answer_context,
           sizeof(uart_quiz_answer_context) - 1);
    memcpy(message + sizeof(uart_quiz_answer_context) - 1, answers,
           BADGE_CTF_UART_QUIZ_ANSWER_COUNT);
    if (hmac_sha256(quiz_key, message, sizeof(message), submitted_tag) !=
        PICO_OK) {
        goto done;
    }
    if (!constant_time_equal(submitted_tag, record->expected_tag,
                             sizeof(submitted_tag))) {
        result = BADGE_UART_QUIZ_INCORRECT;
        goto done;
    }
    result = BADGE_UART_QUIZ_CORRECT;

done:
    zero_bytes(quiz_key, sizeof(quiz_key));
    zero_bytes(submitted_tag, sizeof(submitted_tag));
    zero_bytes(message, sizeof(message));
    return result;
}

bool badge_ctf_uart2_available(void) {
    return uart2_available;
}

static bool submit_fixed_flag(
    const char *submitted_flag,
    const uint8_t expected_digest[SHA256_RESULT_BYTES], bool *solved,
    uint8_t progress_page, const char *name, bool *accepted) {
    if (!flag_digest_matches(submitted_flag, expected_digest)) {
        return false;
    }
    if (*solved || save_progress(progress_page)) {
        *solved = true;
        *accepted = true;
        printf("Accepted: %s\n", name);
    } else {
        puts("Flag is correct, but progress could not be saved.");
    }
    return true;
}

bool badge_ctf_submit_flag(const char *submitted_flag) {
    bool accepted = false;
    if (submit_fixed_flag(submitted_flag, i2c1_flag_digest, &i2c1_solved,
                          PROGRESS_PAGE_I2C1_SOLVED, "I2C1", &accepted) ||
        submit_fixed_flag(submitted_flag, i2c2_flag_digest, &i2c2_solved,
                          PROGRESS_PAGE_I2C2_SOLVED, "I2C2", &accepted) ||
        submit_fixed_flag(submitted_flag, gerber_flag_digest, &gerber_solved,
                          PROGRESS_PAGE_GERBER_SOLVED, "Gerber file",
                          &accepted) ||
        submit_fixed_flag(submitted_flag, schematic_flag_digest,
                          &schematic_solved,
                          PROGRESS_PAGE_SCHEMATIC_SOLVED, "schematic",
                          &accepted)) {
        return accepted;
    }

    char expected_flag[BADGE_CTF_FLAG_TEXT_SIZE] = {0};
    int result = badge_ctf_make_flag(BADGE_FLAG_UART2, expected_flag);

    if (uart2_available && result == PICO_OK &&
        !strcmp(submitted_flag, expected_flag)) {
        if (uart2_solved || save_progress(PROGRESS_PAGE_UART2_SOLVED)) {
            uart2_solved = true;
            accepted = true;
            puts("Accepted: UART2");
        } else {
            puts("Flag is correct, but progress could not be saved.");
        }
        zero_bytes(expected_flag, sizeof(expected_flag));
        return accepted;
    }

    zero_bytes(expected_flag, sizeof(expected_flag));
    result = badge_ctf_make_flag(BADGE_FLAG_UART1, expected_flag);

    if (result == PICO_OK && !strcmp(submitted_flag, expected_flag)) {
        if (uart1_solved || save_progress(PROGRESS_PAGE_UART1_SOLVED)) {
            uart1_solved = true;
            accepted = true;
            puts("Accepted: UART1");
        } else {
            puts("Flag is correct, but progress could not be saved.");
        }
        zero_bytes(expected_flag, sizeof(expected_flag));
        return accepted;
    }

    zero_bytes(expected_flag, sizeof(expected_flag));
    result = badge_ctf_make_flag(BADGE_FLAG_WELCOME, expected_flag);

    if (result == 1) {
        puts("CTF: badge is not provisioned yet.");
    } else if (result != 0) {
        printf("CTF: welcome flag unavailable (%d).\n", result);
    } else if (!strcmp(submitted_flag, expected_flag)) {
        if (welcome_solved || save_progress(PROGRESS_PAGE_WELCOME)) {
            welcome_solved = true;
            accepted = true;
            puts("Accepted: welcome");
        } else {
            puts("Flag is correct, but progress could not be saved.");
        }
    } else {
        zero_bytes(expected_flag, sizeof(expected_flag));
        result = badge_ctf_make_flag(BADGE_FLAG_GPIO1, expected_flag);
        if (result == PICO_OK && !strcmp(submitted_flag, expected_flag)) {
            if (gpio1_solved || save_progress(PROGRESS_PAGE_GPIO1_SOLVED)) {
                gpio1_solved = true;
                accepted = true;
                puts("Accepted: GPIO1");
            } else {
                puts("Flag is correct, but progress could not be saved.");
            }
        } else {
            zero_bytes(expected_flag, sizeof(expected_flag));
            result = badge_ctf_make_flag(BADGE_FLAG_GPIO3, expected_flag);
            if (result == PICO_OK && !strcmp(submitted_flag, expected_flag)) {
                if (gpio3_solved || save_progress(PROGRESS_PAGE_GPIO3_SOLVED)) {
                    gpio3_solved = true;
                    accepted = true;
                    puts("Accepted: GPIO3");
                } else {
                    puts("Flag is correct, but progress could not be saved.");
                }
            } else {
                if (badge_leds_weird_1_matches(submitted_flag)) {
                    if (neopixel1_solved ||
                        save_progress(PROGRESS_PAGE_NEOPIXEL1_SOLVED)) {
                        neopixel1_solved = true;
                        accepted = true;
                        puts("Accepted: NEOPIXEL1");
                    } else {
                        puts("Flag is correct, but progress could not be saved.");
                    }
                } else if (badge_leds_weird_2_matches(submitted_flag)) {
                    if (neopixel2_solved ||
                        save_progress(PROGRESS_PAGE_NEOPIXEL2_SOLVED)) {
                        neopixel2_solved = true;
                        accepted = true;
                        puts("Accepted: NEOPIXEL2");
                    } else {
                        puts("Flag is correct, but progress could not be saved.");
                    }
                } else if (gpio_levels_solved == BADGE_CTF_GPIO_LEVEL_COUNT) {
                    zero_bytes(expected_flag, sizeof(expected_flag));
                    result = badge_ctf_make_flag(BADGE_FLAG_GPIO2, expected_flag);
                    if (result != PICO_OK) {
                        printf("CTF: GPIO2 flag unavailable (%d).\n", result);
                    } else if (!strcmp(submitted_flag, expected_flag)) {
                        if (gpio2_solved ||
                            save_progress(PROGRESS_PAGE_GPIO2_SOLVED)) {
                            gpio2_solved = true;
                            accepted = true;
                            puts("Accepted: GPIO2");
                        } else {
                            puts("Flag is correct, but progress could not be saved.");
                        }
                    } else {
                        puts("Invalid flag.");
                    }
                } else {
                    puts("Invalid flag.");
                }
            }
        }
    }

    zero_bytes(expected_flag, sizeof(expected_flag));
    return accepted;
}

bool badge_ctf_welcome_solved(void) {
    return welcome_solved;
}

bool badge_ctf_gpio1_solved(void) {
    return gpio1_solved;
}

bool badge_ctf_gpio2_solved(void) {
    return gpio2_solved;
}

bool badge_ctf_gpio3_solved(void) {
    return gpio3_solved;
}

bool badge_ctf_neopixel1_solved(void) {
    return neopixel1_solved;
}

bool badge_ctf_neopixel2_solved(void) {
    return neopixel2_solved;
}

bool badge_ctf_uart1_solved(void) {
    return uart1_solved;
}

bool badge_ctf_uart2_solved(void) {
    return uart2_solved;
}

bool badge_ctf_i2c1_solved(void) {
    return i2c1_solved;
}

bool badge_ctf_i2c2_solved(void) {
    return i2c2_solved;
}

bool badge_ctf_gerber_solved(void) {
    return gerber_solved;
}

bool badge_ctf_schematic_solved(void) {
    return schematic_solved;
}

uint8_t badge_ctf_gpio_levels_solved(void) {
    return gpio_levels_solved;
}

uint8_t badge_ctf_solved_count(void) {
    return welcome_solved + neopixel1_solved + neopixel2_solved +
           gpio1_solved + gpio2_solved + gpio3_solved + uart1_solved +
           uart2_solved + i2c1_solved + i2c2_solved + gerber_solved +
           schematic_solved;
}
