#ifndef BADGE_HARDWARE_H
#define BADGE_HARDWARE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef BADGE_LED_COUNT
#define BADGE_LED_COUNT 32
#endif

enum {
    BADGE_FRAME_DELAY_MS = 30,
    BADGE_BUTTON_DEBOUNCE_MS = 200,
    BADGE_GPIO_BRIDGE_COUNT = 4,
    BADGE_I2C_MAX_TRANSFER = 32,
};

typedef enum {
    BADGE_GPIO_LOW,
    BADGE_GPIO_HIGH,
    BADGE_GPIO_FLOATING,
    BADGE_GPIO_INVALID,
} badge_gpio_state_t;

typedef enum {
    BADGE_I2C_PORT_LEFT,
    BADGE_I2C_PORT_RIGHT,
    BADGE_I2C_PORT_COUNT,
} badge_i2c_port_t;

typedef enum {
    LED_ANIMATION_RAINBOW,
    LED_ANIMATION_FED_LED,
    LED_ANIMATION_WEIRD_1,
    LED_ANIMATION_WEIRD_2,
    LED_ANIMATION_JADE_OFF,
    LED_ANIMATION_ORANGE_RED,
    LED_ANIMATION_ENGAGE,
    LED_ANIMATION_ACCESS,
    LED_ANIMATION_AGENCY,
    LED_ANIMATION_SURF_N_TURF,
    LED_ANIMATION_SLOW_RAINBOW_ORBIT,
    LED_ANIMATION_DUAL_COMET_CHASE,
    LED_ANIMATION_BREATHING_COLOR_CYCLE,
    LED_ANIMATION_OPPOSING_COLOR_WAVES,
    LED_ANIMATION_ENERGY_PULSE,
    LED_ANIMATION_MOLTEN_FIRE,
    LED_ANIMATION_POLICE_CYBER_ALERT,
    LED_ANIMATION_AURORA_FLOW,
    LED_ANIMATION_CHARGING_METER,
    LED_ANIMATION_RANDOM_TRAVELING_SPARKS,
    LED_ANIMATION_OFF,
    LED_ANIMATION_COUNT,
} led_animation_t;

typedef struct {
    uint16_t frequency_hz;
    uint16_t duration_ms;
} badge_tone_t;

void badge_hardware_init(void);
bool badge_button_pressed(void);
bool badge_gpio12_grounded(void);
void badge_gpio12_uart_start(void);
void badge_gpio12_uart_write(const char *data, size_t length);
void badge_gpio12_uart_stop(void);
bool badge_uart1_readable(void);
void badge_uart1_console_init(void);
char badge_uart1_read(void);
void badge_uart1_write(const char *data, size_t length);
int badge_i2c_read_register(badge_i2c_port_t port, uint8_t address, uint8_t reg,
                            uint8_t *data, size_t length);
int badge_i2c_write_register(badge_i2c_port_t port, uint8_t address, uint8_t reg,
                             const uint8_t *data, size_t length);
int badge_i2c_probe(badge_i2c_port_t port, uint8_t address);
void badge_gpio_read_bridges(
    badge_gpio_state_t states[BADGE_GPIO_BRIDGE_COUNT]);
bool badge_leds_weird_1_matches(const char *value);
bool badge_leds_weird_2_matches(const char *value);
void badge_leds_select(led_animation_t selected);
void badge_leds_next(uint8_t available_count);
const char *badge_leds_animation_name(led_animation_t selected);
const char *badge_leds_name(void);
void badge_leds_set_brightness(uint8_t percent);
void badge_leds_tick(void);
void badge_play_tune(const badge_tone_t *tune, size_t tone_count);

#endif
