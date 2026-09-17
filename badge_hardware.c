#include "badge_hardware.h"

#include <stdint.h>
#include <math.h>
#include <string.h>

#include "hardware/i2c.h"
#include "hardware/pio.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#include "ws2812.pio.h"

enum {
    NUM_PIXELS = BADGE_LED_COUNT,
    PIXEL_PIN = 1,
    BUTTON_PIN = 23,
    PIEZO_PIN_A = 6,
    PIEZO_PIN_B = 8,
    UART1_TX_PIN = 4,
    UART1_RX_PIN = 5,
    UART1_BAUD = 115200,
    I2C_LEFT_SDA_PIN = 10,
    I2C_LEFT_SCL_PIN = 11,
    I2C_RIGHT_SDA_PIN = 24,
    I2C_RIGHT_SCL_PIN = 25,
    I2C_BAUD = 400000,
    I2C_TIMEOUT_US = 100000,
    I2C_SCAN_TIMEOUT_US = 2000,
    GPIO12_PIN = 12,
    GPIO12_UART_BAUD = 9600,
    WEIRD_FRAMES_PER_PIXEL = 4,
    WEIRD_2_ON_FRAMES = 20,
    WEIRD_2_CYCLE_FRAMES = 30,
};

static const uint gpio_bridge_pins[BADGE_GPIO_BRIDGE_COUNT] = {19, 20, 21, 22};

static const char *const animation_names[] = {
    "Rainbow", "Fed Led", "Weird 1", "Weird 2", "Money Bags",
    "Apples And Oranges", "Engage", "Access", "Agency", "Surf N Turf",
    "Slow Rainbow Orbit", "Dual Comet Chase", "Breathing Rgb", "Color Waves",
    "Energy Pulse", "Molten Fire", "Cyber Alert", "Aurora Flow",
    "Color Charger", "Sparks", "Off"
};
_Static_assert(sizeof(animation_names) / sizeof(animation_names[0]) == LED_ANIMATION_COUNT,
               "animation names must match animation enum");

static PIO led_pio;
static uint led_sm;
static led_animation_t animation = LED_ANIMATION_RAINBOW;
static uint16_t animation_step;
static uint32_t effect_step;
static uint32_t spark_random = 0x71c4a39du;
static struct {
    float position;
    float speed;
    uint16_t age;
    uint16_t lifetime;
    uint8_t color;
} sparks[3];
static uint8_t brightness_percent = 25;
static const uint8_t engage_palette[][3] = {
    {0xFF, 0x78, 0x00}, {0xD8, 0x08, 0xE8},
    {0xFF, 0x10, 0x48}, {0x00, 0xCF, 0x88},
};
static const uint8_t access_palette[][3] = {
    {0x40, 0x58, 0xE8}, {0x00, 0xEE, 0x98}, {0x90, 0xEE, 0x00},
    {0xFF, 0x98, 0x00}, {0xFF, 0x40, 0x18}, {0xFF, 0x08, 0x68},
};
static const uint8_t agency_palette[][3] = {
    {0x28, 0x38, 0xC8}, {0x00, 0xB8, 0xD0}, {0x00, 0xCC, 0x68},
    {0x30, 0xFF, 0x70}, {0xFF, 0x88, 0x00}, {0xFF, 0x18, 0x28},
};
static const uint8_t surf_n_turf_palette[][3] = {
    {0xFF, 0x68, 0x08}, {0xFF, 0x40, 0x00}, {0xFF, 0x20, 0x08}, {0xFF, 0x80, 0x00},
    {0xB0, 0xD8, 0x00}, {0x68, 0xD0, 0x00}, {0x00, 0xE0, 0x60}, {0x00, 0xFF, 0x90},
};
static const uint8_t weird_1_rgb[] = {
    0x46, 0x4c, 0x41, 0x47, 0x7b, 0x4e, 0x45, 0x4f, 0x50, 0x49, 0x58, 0x45,
    0x4c, 0x31, 0x5f, 0x66, 0x64, 0x34, 0x35, 0x37, 0x38, 0x62, 0x30, 0x66,
    0x33, 0x36, 0x32, 0x32, 0x64, 0x34, 0x63, 0x61, 0x32, 0x64, 0x62, 0x65,
    0x63, 0x38, 0x61, 0x39, 0x65, 0x32, 0x36, 0x31, 0x30, 0x66, 0x36, 0x7d,
};
static const uint8_t weird_2_data[] = {
    0x4c, 0x46, 0x41, 0x7b, 0x47, 0x4e, 0x4f, 0x45, 0x50, 0x58, 0x49, 0x45,
    0x32, 0x4c, 0x5f, 0x39, 0x34, 0x30, 0x31, 0x38, 0x65, 0x36, 0x37, 0x64,
    0x31, 0x31, 0x37, 0x31, 0x61, 0x38, 0x34, 0x32, 0x61, 0x35, 0x64, 0x31,
    0x30, 0x63, 0x64, 0x38, 0x30, 0x38, 0x62, 0x63, 0x66, 0x38, 0x34, 0x7d,
};

static void configure_gpio12_input(void) {
    gpio_init(GPIO12_PIN);
    gpio_set_dir(GPIO12_PIN, GPIO_IN);
    gpio_pull_up(GPIO12_PIN);
}

static void put_pixel_bytes(uint8_t first, uint8_t second, uint8_t third) {
    pio_sm_put_blocking(led_pio, led_sm,
                        ((uint32_t)first << 24) | ((uint32_t)second << 16) |
                            ((uint32_t)third << 8));
}

static void put_pixel(uint8_t red, uint8_t green, uint8_t blue) {
    red = (uint8_t)((uint16_t)red * brightness_percent / 100);
    green = (uint8_t)((uint16_t)green * brightness_percent / 100);
    blue = (uint8_t)((uint16_t)blue * brightness_percent / 100);
    put_pixel_bytes(green, red, blue);
}

static void put_pixel_unscaled(uint8_t red, uint8_t green, uint8_t blue) {
    put_pixel_bytes(green, red, blue);
}

static void color_wheel(uint8_t position, uint8_t *red, uint8_t *green, uint8_t *blue) {
    if (position < 85) {
        *red = position * 3;
        *green = 255 - position * 3;
        *blue = 0;
    } else if (position < 170) {
        position -= 85;
        *red = 255 - position * 3;
        *green = 0;
        *blue = position * 3;
    } else {
        position -= 170;
        *red = 0;
        *green = position * 3;
        *blue = 255 - position * 3;
    }
}

static void put_palette_pixel(const uint8_t palette[][3], size_t count, uint pixel) {
    uint phase = pixel * 256 / NUM_PIXELS + animation_step * 4;
    uint level = 255;
    if (animation == LED_ANIMATION_ENGAGE) {
        uint tail = (animation_step / 2 + NUM_PIXELS - pixel) % 8;
        level = 255 - tail * 28;
        phase = pixel / 8 * 64 + animation_step * 2;
    } else if (animation == LED_ANIMATION_ACCESS) {
        uint travel = animation_step / 2;
        uint lane = pixel / 8;
        uint tail = (lane & 1u) ? (pixel + travel) % 8
                                    : (travel + NUM_PIXELS - pixel) % 8;
        level = tail < 3 ? 255 - tail * 55 : 56;
        phase = lane * 43 + animation_step * 5;
    } else if (animation == LED_ANIMATION_AGENCY) {
        uint travel = (animation_step / 2) % (2 * (NUM_PIXELS - 1));
        uint head = travel < NUM_PIXELS ? travel : 2 * (NUM_PIXELS - 1) - travel;
        uint distance = pixel > head ? pixel - head : head - pixel;
        level = distance < 6 ? 255 - distance * 38 : 56;
        phase = pixel * 8 + animation_step * 3;
    } else if (animation == LED_ANIMATION_SURF_N_TURF) {
        uint wave = (pixel * 16 + 256 - (animation_step * 8 & 255u)) & 255u;
        uint crest = wave < 128 ? wave : 255 - wave;
        level = 56 + crest * 199 / 127;
        phase = pixel * 8 + animation_step * 6;
    }
    phase &= 255u;
    uint position = phase * count;
    uint index = position / 256;
    uint blend = position % 256;
    blend = blend < 64 ? 0 : blend >= 192 ? 256 : (blend - 64) * 2;
    uint8_t rgb[3];
    for (uint channel = 0; channel < 3; ++channel) {
        rgb[channel] = (palette[index][channel] * (256 - blend) +
                        palette[(index + 1) % count][channel] * blend) / 256;
    }
    uint white = rgb[0] < rgb[1] ? rgb[0] : rgb[1];
    if (rgb[2] < white) white = rgb[2];
    white = white * 3 / 4;
    for (uint channel = 0; channel < 3; ++channel) {
        rgb[channel] = (rgb[channel] - white) * level / 255;
    }
    put_pixel(rgb[0], rgb[1], rgb[2]);
}

static float smooth_level(float value) {
    if (value <= 0.0f) return 0.0f;
    if (value >= 1.0f) return 1.0f;
    return value * value * (3.0f - 2.0f * value);
}

static float effect_phase(uint frames) {
    return (effect_step % frames) / (float)frames;
}

static float glow_wave(float phase) {
    return 0.5f - 0.5f * cosf(6.2831853f * phase);
}

static void gradient_color(const uint8_t palette[][3], uint count, float phase,
                           float rgb[3]) {
    float position = (phase - floorf(phase)) * count;
    uint index = (uint)position;
    float blend = smooth_level(position - index);
    for (uint channel = 0; channel < 3; ++channel) {
        rgb[channel] = palette[index][channel] * (1.0f - blend) +
                       palette[(index + 1) % count][channel] * blend;
    }
}

static float comet_glow(float pixel, float head, float tail) {
    float distance = fmodf(head - pixel + NUM_PIXELS, NUM_PIXELS);
    float rear = smooth_level(1.0f - distance / tail);
    float front = smooth_level(1.0f - (NUM_PIXELS - distance) / 2.5f);
    return rear > front ? rear : front;
}

static uint next_spark_random(void) {
    spark_random ^= spark_random << 13;
    spark_random ^= spark_random >> 17;
    spark_random ^= spark_random << 5;
    return spark_random;
}

static void reset_spark(uint index) {
    sparks[index].position = (next_spark_random() % (NUM_PIXELS * 256)) / 256.0f;
    sparks[index].speed = 0.07f + index * 0.035f + (next_spark_random() % 20) * 0.001f;
    sparks[index].age = 0;
    sparks[index].lifetime = 160 + next_spark_random() % 120;
    sparks[index].color = next_spark_random() % 3;
}

static void render_diffused_pixel(uint pixel) {
    static const uint8_t rainbow[][3] = {
        {255, 0, 0}, {255, 160, 0}, {0, 255, 0},
        {0, 220, 255}, {0, 0, 255}, {190, 0, 255},
    };
    static const uint8_t breathing[][3] = {
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255},
    };
    static const uint8_t aurora[][3] = {
        {8, 16, 210}, {0, 180, 200}, {0, 210, 65}, {115, 0, 200},
    };
    static const uint8_t spark_colors[][3] = {
        {255, 95, 4}, {255, 30, 12}, {0, 160, 210},
    };
    float x = pixel / (float)NUM_PIXELS;
    float rgb[3] = {0.0f, 0.0f, 0.0f};
    float level = 1.0f;
    switch (animation) {
        case LED_ANIMATION_SLOW_RAINBOW_ORBIT:
            gradient_color(rainbow, 6, x - effect_phase(600), rgb);
            break;
        case LED_ANIMATION_DUAL_COMET_CHASE: {
            float head = effect_phase(240) * NUM_PIXELS;
            float first = comet_glow(pixel, head, 7.0f);
            float second = comet_glow(pixel, fmodf(head + NUM_PIXELS * 0.5f, NUM_PIXELS), 7.0f);
            rgb[0] = 255.0f * first;
            rgb[1] = 4.0f + 80.0f * first + 140.0f * second;
            rgb[2] = 8.0f + 210.0f * second;
            break;
        }
        case LED_ANIMATION_BREATHING_COLOR_CYCLE: {
            float phase = effect_phase(160);
            uint color = (effect_step / 160) % 3;
            float blend = smooth_level(phase / 0.12f);
            for (uint channel = 0; channel < 3; ++channel) {
                rgb[channel] = breathing[(color + 2) % 3][channel] * (1.0f - blend) +
                               breathing[color][channel] * blend;
            }
            level = 0.04f + 0.96f * glow_wave(phase);
            break;
        }
        case LED_ANIMATION_OPPOSING_COLOR_WAVES: {
            float blue = glow_wave(x - effect_phase(240));
            float magenta = glow_wave(x + effect_phase(300));
            rgb[0] = 200.0f * magenta;
            rgb[1] = 8.0f * blue;
            rgb[2] = 120.0f * blue + 100.0f * magenta;
            break;
        }
        case LED_ANIMATION_ENERGY_PULSE: {
            float phase = effect_phase(160);
            float distance = x < 0.5f ? x : 1.0f - x;
            float radius = phase * 0.7f;
            float pulse = smooth_level(1.0f - fabsf(distance - radius) / 0.15f);
            pulse *= smooth_level(phase / 0.08f) * (1.0f - smooth_level(phase));
            rgb[0] = 2.0f + 60.0f * pulse;
            rgb[1] = 5.0f + 245.0f * pulse;
            rgb[2] = 12.0f + 220.0f * pulse;
            break;
        }
        case LED_ANIMATION_MOLTEN_FIRE: {
            float heat = 0.65f * glow_wave(x - effect_phase(700)) +
                         0.35f * glow_wave(2.0f * x + effect_phase(430));
            rgb[0] = 90.0f + 165.0f * heat;
            rgb[1] = 110.0f * heat * heat * heat;
            rgb[2] = 0.0f;
            break;
        }
        case LED_ANIMATION_POLICE_CYBER_ALERT: {
            float phase = effect_phase(120);
            float pulse = glow_wave(phase + (pixel < NUM_PIXELS / 2 ? 0.0f : 0.5f));
            rgb[pixel < NUM_PIXELS / 2 ? 2 : 0] = 255.0f * (0.03f + 0.97f * pulse);
            uint accent_frame = effect_step % 480;
            if (accent_frame >= 460) {
                float accent = glow_wave((accent_frame - 460) / 20.0f);
                for (uint channel = 0; channel < 3; ++channel) {
                    rgb[channel] = rgb[channel] * (1.0f - accent) + 32.0f * accent;
                }
            }
            break;
        }
        case LED_ANIMATION_AURORA_FLOW:
            gradient_color(aurora, 4, x - effect_phase(800) +
                           0.08f * glow_wave(x + effect_phase(1100)), rgb);
            level = 0.25f + 0.75f * glow_wave(x + effect_phase(600));
            break;
        case LED_ANIMATION_CHARGING_METER: {
            uint frame = effect_step % 400;
            float progress;
            float confirmation = 0.65f;
            if (frame < 200) {
                progress = smooth_level(frame / 200.0f);
            } else if (frame < 260) {
                progress = 1.0f;
                confirmation += 0.35f * glow_wave((frame - 200) / 60.0f);
            } else {
                progress = 1.0f - smooth_level((frame - 260) / 120.0f);
            }
            float edge = smooth_level((progress * (NUM_PIXELS + 2) - pixel) / 3.0f);
            rgb[0] = progress < 0.5f ? 255.0f : 510.0f * (1.0f - progress);
            rgb[1] = progress < 0.5f ? 510.0f * progress : 255.0f;
            level = edge * confirmation;
            break;
        }
        case LED_ANIMATION_RANDOM_TRAVELING_SPARKS:
            rgb[0] = 5.0f;
            rgb[1] = 2.0f;
            rgb[2] = 8.0f;
            for (uint i = 0; i < sizeof(sparks) / sizeof(sparks[0]); ++i) {
                float glow = comet_glow(pixel, sparks[i].position, 7.0f) *
                             glow_wave(sparks[i].age / (float)sparks[i].lifetime);
                for (uint channel = 0; channel < 3; ++channel) {
                    rgb[channel] += spark_colors[sparks[i].color][channel] * glow;
                }
            }
            break;
        default:
            break;
    }
    float peak = fmaxf(rgb[0], fmaxf(rgb[1], rgb[2]));
    if (peak > 255.0f) level *= 255.0f / peak;
    put_pixel((uint8_t)(rgb[0] * level), (uint8_t)(rgb[1] * level),
              (uint8_t)(rgb[2] * level));
}

static void render_animation(void) {
    for (uint pixel = 0; pixel < NUM_PIXELS; ++pixel) {
        if (animation == LED_ANIMATION_FED_LED) {
            bool red = ((pixel + animation_step / 8) & 1u) == 0;
            put_pixel(red ? 255 : 0, 0, red ? 0 : 255);
        } else if (animation == LED_ANIMATION_RAINBOW) {
            uint8_t red, green, blue;
            color_wheel((uint8_t)(pixel * 256 / NUM_PIXELS + animation_step * 3),
                        &red, &green, &blue);
            put_pixel(red, green, blue);
        } else if (animation == LED_ANIMATION_WEIRD_1) {
            uint offset =
                (animation_step / WEIRD_FRAMES_PER_PIXEL) % NUM_PIXELS;
            uint source_pixel = (pixel + NUM_PIXELS - offset) % NUM_PIXELS;
            if (source_pixel < sizeof(weird_1_rgb) / 3) {
                const uint8_t *rgb = &weird_1_rgb[source_pixel * 3];
                put_pixel_unscaled(rgb[0], rgb[1], rgb[2]);
            } else {
                put_pixel_unscaled(0, 0, 0);
            }
        } else if (animation == LED_ANIMATION_WEIRD_2) {
            bool visible =
                animation_step % WEIRD_2_CYCLE_FRAMES < WEIRD_2_ON_FRAMES;
            if (visible && pixel < sizeof(weird_2_data) / 3) {
                const uint8_t *data = &weird_2_data[pixel * 3];
                put_pixel_bytes(data[0], data[1], data[2]);
            } else {
                put_pixel_bytes(0, 0, 0);
            }
        } else if (animation == LED_ANIMATION_JADE_OFF) {
            bool jade = ((pixel + animation_step / 8) & 1u) == 0;
            put_pixel(0, jade ? 168 : 0, jade ? 107 : 0);
        } else if (animation == LED_ANIMATION_ORANGE_RED) {
            bool orange = ((pixel + animation_step / 8) & 1u) == 0;
            put_pixel(255, orange ? 165 : 0, 0);
        } else if (animation == LED_ANIMATION_ENGAGE) {
            put_palette_pixel(engage_palette,
                              sizeof(engage_palette) / sizeof(engage_palette[0]), pixel);
        } else if (animation == LED_ANIMATION_ACCESS) {
            put_palette_pixel(access_palette,
                              sizeof(access_palette) / sizeof(access_palette[0]), pixel);
        } else if (animation == LED_ANIMATION_AGENCY) {
            put_palette_pixel(agency_palette,
                              sizeof(agency_palette) / sizeof(agency_palette[0]), pixel);
        } else if (animation == LED_ANIMATION_SURF_N_TURF) {
            put_palette_pixel(surf_n_turf_palette,
                              sizeof(surf_n_turf_palette) / sizeof(surf_n_turf_palette[0]), pixel);
        } else if (animation >= LED_ANIMATION_SLOW_RAINBOW_ORBIT &&
                   animation < LED_ANIMATION_OFF) {
            render_diffused_pixel(pixel);
        } else {
            put_pixel(0, 0, 0);
        }
    }
}

static void play_tone(uint32_t frequency, uint32_t duration_ms) {
    uint32_t half_period_us = 500000 / frequency;
    uint32_t cycles = frequency * duration_ms / 1000;

    for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
        gpio_put(PIEZO_PIN_A, true);
        gpio_put(PIEZO_PIN_B, false);
        busy_wait_us_32(half_period_us);
        gpio_put(PIEZO_PIN_A, false);
        gpio_put(PIEZO_PIN_B, true);
        busy_wait_us_32(half_period_us);
    }

    gpio_put(PIEZO_PIN_A, false);
    gpio_put(PIEZO_PIN_B, false);
}

void badge_play_tune(const badge_tone_t *tune, size_t tone_count) {
    for (size_t i = 0; i < tone_count; ++i) {
        if (tune[i].frequency_hz) {
            play_tone(tune[i].frequency_hz, tune[i].duration_ms);
        } else {
            sleep_ms(tune[i].duration_ms);
        }
    }
}

void badge_hardware_init(void) {
    uint led_offset;
    if (!pio_claim_free_sm_and_add_program_for_gpio_range(
            &ws2812_program, &led_pio, &led_sm, &led_offset, PIXEL_PIN, 1, true)) {
        panic("No PIO state machine available for NeoPixels");
    }
    ws2812_program_init(led_pio, led_sm, led_offset, PIXEL_PIN, 800000, false);

    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN);

    configure_gpio12_input();

    badge_uart1_console_init();

    i2c_init(i2c1, I2C_BAUD);
    gpio_set_function(I2C_LEFT_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_LEFT_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_LEFT_SDA_PIN);
    gpio_pull_up(I2C_LEFT_SCL_PIN);

    i2c_init(i2c0, I2C_BAUD);
    gpio_set_function(I2C_RIGHT_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_RIGHT_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_RIGHT_SDA_PIN);
    gpio_pull_up(I2C_RIGHT_SCL_PIN);

    gpio_init(PIEZO_PIN_A);
    gpio_init(PIEZO_PIN_B);
    gpio_set_dir(PIEZO_PIN_A, GPIO_OUT);
    gpio_set_dir(PIEZO_PIN_B, GPIO_OUT);
    gpio_put(PIEZO_PIN_A, false);
    gpio_put(PIEZO_PIN_B, false);

    render_animation();
}

void badge_uart1_console_init(void) {
    uart_deinit(uart1);
    uart_init(uart1, UART1_BAUD);
    uart_set_format(uart1, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(uart1, false, false);
    uart_set_fifo_enabled(uart1, true);
    gpio_set_function(UART1_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART1_RX_PIN, GPIO_FUNC_UART);
}

bool badge_button_pressed(void) {
    return !gpio_get(BUTTON_PIN);
}

bool badge_gpio12_grounded(void) {
    return !gpio_get(GPIO12_PIN);
}

void badge_gpio12_uart_start(void) {
    gpio_disable_pulls(GPIO12_PIN);
    uart_init(uart0, GPIO12_UART_BAUD);
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(uart0, false, false);
    uart_set_fifo_enabled(uart0, true);
    gpio_set_function(GPIO12_PIN, GPIO_FUNC_UART);
}

void badge_gpio12_uart_write(const char *data, size_t length) {
    uart_write_blocking(uart0, (const uint8_t *)data, length);
}

void badge_gpio12_uart_stop(void) {
    uart_deinit(uart0);
    configure_gpio12_input();
}

bool badge_uart1_readable(void) {
    return uart_is_readable(uart1);
}

char badge_uart1_read(void) {
    return (char)uart_getc(uart1);
}

void badge_uart1_write(const char *data, size_t length) {
    uart_write_blocking(uart1, (const uint8_t *)data, length);
}

static i2c_inst_t *badge_i2c_instance(badge_i2c_port_t port) {
    if (port == BADGE_I2C_PORT_LEFT) {
        return i2c1;
    }
    if (port == BADGE_I2C_PORT_RIGHT) {
        return i2c0;
    }
    return NULL;
}

int badge_i2c_read_register(badge_i2c_port_t port, uint8_t address, uint8_t reg,
                            uint8_t *data, size_t length) {
    i2c_inst_t *i2c = badge_i2c_instance(port);
    if (!i2c || !data || !length || length > BADGE_I2C_MAX_TRANSFER) {
        return PICO_ERROR_GENERIC;
    }
    int result = i2c_write_timeout_us(i2c, address, &reg, 1, true,
                                      I2C_TIMEOUT_US);
    if (result != 1) {
        return result < 0 ? result : PICO_ERROR_GENERIC;
    }
    return i2c_read_timeout_us(i2c, address, data, length, false,
                               I2C_TIMEOUT_US);
}

int badge_i2c_write_register(badge_i2c_port_t port, uint8_t address, uint8_t reg,
                             const uint8_t *data, size_t length) {
    uint8_t transfer[BADGE_I2C_MAX_TRANSFER + 1];
    i2c_inst_t *i2c = badge_i2c_instance(port);
    if (!i2c || !data || !length || length > BADGE_I2C_MAX_TRANSFER) {
        return PICO_ERROR_GENERIC;
    }
    transfer[0] = reg;
    memcpy(transfer + 1, data, length);
    return i2c_write_timeout_us(i2c, address, transfer, length + 1, false,
                                I2C_TIMEOUT_US);
}

int badge_i2c_probe(badge_i2c_port_t port, uint8_t address) {
    uint8_t ignored;
    i2c_inst_t *i2c = badge_i2c_instance(port);
    return i2c ? i2c_read_timeout_us(i2c, address, &ignored, 1, false,
                                     I2C_SCAN_TIMEOUT_US)
               : PICO_ERROR_GENERIC;
}

void badge_gpio_read_bridges(
    badge_gpio_state_t states[BADGE_GPIO_BRIDGE_COUNT]) {
    bool pulled_down[BADGE_GPIO_BRIDGE_COUNT];

    for (size_t i = 0; i < BADGE_GPIO_BRIDGE_COUNT; ++i) {
        gpio_init(gpio_bridge_pins[i]);
        gpio_set_dir(gpio_bridge_pins[i], GPIO_IN);
        gpio_pull_down(gpio_bridge_pins[i]);
    }
    sleep_us(1000);
    for (size_t i = 0; i < BADGE_GPIO_BRIDGE_COUNT; ++i) {
        pulled_down[i] = gpio_get(gpio_bridge_pins[i]);
        gpio_pull_up(gpio_bridge_pins[i]);
    }
    sleep_us(1000);
    for (size_t i = 0; i < BADGE_GPIO_BRIDGE_COUNT; ++i) {
        bool pulled_up = gpio_get(gpio_bridge_pins[i]);
        states[i] = pulled_down[i]
                        ? (pulled_up ? BADGE_GPIO_HIGH : BADGE_GPIO_INVALID)
                        : (pulled_up ? BADGE_GPIO_FLOATING : BADGE_GPIO_LOW);
        gpio_disable_pulls(gpio_bridge_pins[i]);
    }
}

bool badge_leds_weird_1_matches(const char *value) {
    return value && strlen(value) == sizeof(weird_1_rgb) &&
           !memcmp(value, weird_1_rgb, sizeof(weird_1_rgb));
}

bool badge_leds_weird_2_matches(const char *value) {
    if (!value || strlen(value) != sizeof(weird_2_data)) {
        return false;
    }
    for (size_t i = 0; i < sizeof(weird_2_data); i += 3) {
        if ((uint8_t)value[i] != weird_2_data[i + 1] ||
            (uint8_t)value[i + 1] != weird_2_data[i] ||
            (uint8_t)value[i + 2] != weird_2_data[i + 2]) {
            return false;
        }
    }
    return true;
}

void badge_leds_select(led_animation_t selected) {
    animation = selected;
    animation_step = 0;
    effect_step = 0;
    if (selected == LED_ANIMATION_RANDOM_TRAVELING_SPARKS) {
        spark_random = 0x71c4a39du;
        for (uint i = 0; i < sizeof(sparks) / sizeof(sparks[0]); ++i) {
            reset_spark(i);
        }
    }
}

void badge_leds_next(uint8_t available_count) {
    led_animation_t next = animation == LED_ANIMATION_OFF
                               ? LED_ANIMATION_RAINBOW
                               : (led_animation_t)(animation + 1);
    if (next >= available_count && next < LED_ANIMATION_OFF) {
        next = LED_ANIMATION_OFF;
    }
    badge_leds_select(next);
}

const char *badge_leds_animation_name(led_animation_t selected) {
    return animation_names[selected];
}

const char *badge_leds_name(void) {
    return badge_leds_animation_name(animation);
}

void badge_leds_set_brightness(uint8_t percent) {
    brightness_percent = percent;
    render_animation();
}

void badge_leds_tick(void) {
    render_animation();
    ++animation_step;
    if (animation >= LED_ANIMATION_SLOW_RAINBOW_ORBIT &&
        animation < LED_ANIMATION_OFF) {
        ++effect_step;
    }
    if (animation == LED_ANIMATION_RANDOM_TRAVELING_SPARKS) {
        for (uint i = 0; i < sizeof(sparks) / sizeof(sparks[0]); ++i) {
            sparks[i].position = fmodf(sparks[i].position + sparks[i].speed, NUM_PIXELS);
            if (++sparks[i].age >= sparks[i].lifetime) reset_spark(i);
        }
    }
}
