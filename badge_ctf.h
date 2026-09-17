#ifndef BADGE_CTF_H
#define BADGE_CTF_H

#include <stdbool.h>
#include <stdint.h>

enum {
    BADGE_CTF_FLAG_TEXT_SIZE = 64,
    BADGE_CTF_GPIO_LEVEL_COUNT = 4,
    BADGE_CTF_UART_QUIZ_ANSWER_COUNT = 10,
    BADGE_CTF_CHALLENGE_COUNT = 12,
};

typedef enum {
    BADGE_FLAG_WELCOME,
    BADGE_FLAG_GPIO1,
    BADGE_FLAG_GPIO2,
    BADGE_FLAG_GPIO3,
    BADGE_FLAG_UART1,
    BADGE_FLAG_UART2,
    BADGE_FLAG_COUNT,
} badge_flag_id_t;

typedef enum {
    BADGE_UART_QUIZ_UNAVAILABLE = -1,
    BADGE_UART_QUIZ_INCORRECT,
    BADGE_UART_QUIZ_CORRECT,
} badge_uart_quiz_result_t;

void badge_ctf_init(void);
int badge_ctf_make_flag(
    badge_flag_id_t id, char output[BADGE_CTF_FLAG_TEXT_SIZE]);
bool badge_ctf_submit_flag(const char *submitted_flag);
badge_uart_quiz_result_t badge_ctf_submit_uart2_answers(
    const char answers[BADGE_CTF_UART_QUIZ_ANSWER_COUNT]);
bool badge_ctf_uart2_available(void);
bool badge_ctf_welcome_solved(void);
bool badge_ctf_gpio1_solved(void);
bool badge_ctf_gpio2_solved(void);
bool badge_ctf_gpio3_solved(void);
bool badge_ctf_neopixel1_solved(void);
bool badge_ctf_neopixel2_solved(void);
bool badge_ctf_uart1_solved(void);
bool badge_ctf_uart2_solved(void);
bool badge_ctf_i2c1_solved(void);
bool badge_ctf_i2c2_solved(void);
bool badge_ctf_gerber_solved(void);
bool badge_ctf_schematic_solved(void);
uint8_t badge_ctf_gpio_levels_solved(void);
uint8_t badge_ctf_solved_count(void);

#endif
