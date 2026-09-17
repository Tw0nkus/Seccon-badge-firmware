#include "badge_settings.h"

#include <stdint.h>
#include <string.h>

#include "badge_hardware.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "pico/flash.h"
#include "pico/stdlib.h"

enum {
    SETTINGS_MAGIC_BYTES = 8,
    SETTINGS_BRIGHTNESS_OFFSET = SETTINGS_MAGIC_BYTES,
    SETTINGS_STARTUP_TUNE_OFFSET,
    SETTINGS_LEGACY_CHECKSUM_OFFSET,
    SETTINGS_DEFAULT_LED_ANIMATION_OFFSET,
    SETTINGS_CHECKSUM_OFFSET,
    SETTINGS_RECORD_BYTES,
};

typedef struct {
    uint32_t offset;
    const uint8_t *data;
} settings_write_t;

extern char __badge_settings_start;

const uint8_t badge_brightness_choices[BADGE_BRIGHTNESS_CHOICE_COUNT] = {
    25, 50, 75, 100,
};
static const uint8_t settings_magic[] = "BADGSET1";
_Static_assert(sizeof(settings_magic) - 1 == SETTINGS_MAGIC_BYTES,
               "settings magic size must match record layout");

static uint32_t settings_flash_offset(void) {
    return (uint32_t)((uintptr_t)&__badge_settings_start - XIP_BASE);
}

static const uint8_t *settings_flash(void) {
    return (const uint8_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE +
                             settings_flash_offset());
}

static uint8_t settings_checksum(uint8_t brightness, uint8_t startup_tune) {
    return (uint8_t)(brightness ^ startup_tune ^ 0xa5);
}

static uint8_t settings_checksum_with_animation(uint8_t brightness,
                                                uint8_t startup_tune,
                                                uint8_t animation) {
    return (uint8_t)(settings_checksum(brightness, startup_tune) ^ animation ^
                     0x5a);
}

static bool brightness_is_valid(uint8_t brightness) {
    for (size_t i = 0; i < BADGE_BRIGHTNESS_CHOICE_COUNT; ++i) {
        if (brightness == badge_brightness_choices[i]) {
            return true;
        }
    }
    return false;
}

void badge_settings_load(badge_settings_t *settings) {
    const uint8_t *stored = settings_flash();
    uint8_t brightness = stored[SETTINGS_BRIGHTNESS_OFFSET];
    uint8_t startup_tune = stored[SETTINGS_STARTUP_TUNE_OFFSET];
    uint8_t default_animation =
        stored[SETTINGS_DEFAULT_LED_ANIMATION_OFFSET];

    if (!memcmp(stored, settings_magic, SETTINGS_MAGIC_BYTES) &&
        brightness_is_valid(brightness) && startup_tune < BADGE_TUNE_COUNT &&
        stored[SETTINGS_LEGACY_CHECKSUM_OFFSET] ==
            settings_checksum(brightness, startup_tune)) {
        settings->led_brightness_percent = brightness;
        settings->startup_tune = startup_tune;
        if (default_animation < LED_ANIMATION_COUNT &&
            stored[SETTINGS_CHECKSUM_OFFSET] == settings_checksum_with_animation(
                                                    brightness, startup_tune,
                                                    default_animation)) {
            settings->default_led_animation = default_animation;
        }
    }
}

static void write_settings_sector(void *parameter) {
    const settings_write_t *write = parameter;
    flash_range_erase(write->offset, FLASH_SECTOR_SIZE);
    flash_range_program(write->offset, write->data, FLASH_PAGE_SIZE);
}

bool badge_settings_save(const badge_settings_t *settings) {
    uint8_t record[FLASH_PAGE_SIZE];
    memset(record, 0xff, sizeof(record));
    memcpy(record, settings_magic, SETTINGS_MAGIC_BYTES);
    record[SETTINGS_BRIGHTNESS_OFFSET] = settings->led_brightness_percent;
    record[SETTINGS_STARTUP_TUNE_OFFSET] = settings->startup_tune;
    record[SETTINGS_LEGACY_CHECKSUM_OFFSET] = settings_checksum(
        settings->led_brightness_percent, settings->startup_tune);
    record[SETTINGS_DEFAULT_LED_ANIMATION_OFFSET] =
        settings->default_led_animation;
    record[SETTINGS_CHECKSUM_OFFSET] = settings_checksum_with_animation(
        settings->led_brightness_percent, settings->startup_tune,
        settings->default_led_animation);

    settings_write_t write = {
        .offset = settings_flash_offset(),
        .data = record,
    };
    return write.offset % FLASH_SECTOR_SIZE == 0 &&
           flash_safe_execute(write_settings_sector, &write, UINT32_MAX) == PICO_OK &&
           !memcmp(settings_flash(), record, SETTINGS_RECORD_BYTES);
}
