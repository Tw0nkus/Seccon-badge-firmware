#ifndef BADGE_SETTINGS_H
#define BADGE_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "badge_tunes.h"

enum {
    BADGE_BRIGHTNESS_CHOICE_COUNT = 4,
};

typedef struct {
    uint8_t led_brightness_percent;
    uint8_t startup_tune;
    uint8_t default_led_animation;
} badge_settings_t;

extern const uint8_t
    badge_brightness_choices[BADGE_BRIGHTNESS_CHOICE_COUNT];

void badge_settings_load(badge_settings_t *settings);
bool badge_settings_save(const badge_settings_t *settings);

#endif
