#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "badge_ansi.h"
#include "badge_ctf.h"
#include "badge_files.h"
#include "badge_hardware.h"
#include "badge_settings.h"
#include "badge_tunes.h"
#include "badge_updiprog.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

typedef enum {
    MENU_MAIN,
    MENU_LED,
    MENU_TUNES,
    MENU_SETTINGS,
    MENU_SETTINGS_BRIGHTNESS,
    MENU_SETTINGS_STARTUP,
    MENU_SETTINGS_DEFAULT_LED,
    MENU_CTF,
    MENU_WELCOME,
    MENU_NEOPIXEL1,
    MENU_NEOPIXEL2,
    MENU_GPIO1,
    MENU_GPIO2,
    MENU_GPIO3,
    MENU_UART1,
    MENU_UART2,
    MENU_I2C1_CHALLENGE,
    MENU_I2C2_CHALLENGE,
    MENU_GERBER_CHALLENGE,
    MENU_SCHEMATIC_CHALLENGE,
    MENU_UPDI,
    MENU_FILES,
    MENU_I2C,
    MENU_I2C_SCRIPT,
} menu_t;

typedef enum {
    ESCAPE_NONE,
    ESCAPE_STARTED,
    ESCAPE_CSI,
} escape_state_t;

typedef enum {
    UART_CONSOLE_MENU,
    UART_CONSOLE_CHALLENGE_1,
    UART_CONSOLE_CHALLENGE_2,
} uart_console_menu_t;

enum {
    UART_QUIZ_RETRY_DELAY_MS = 1000,
    I2C_LINE_MAX_OPERATIONS = 16,
};

typedef enum {
    I2C_OPERATION_READ,
    I2C_OPERATION_WRITE,
} i2c_operation_kind_t;

typedef struct {
    i2c_operation_kind_t kind;
    badge_i2c_port_t port;
    uint8_t address;
    uint8_t reg;
    uint8_t length;
    uint8_t data[BADGE_I2C_MAX_TRANSFER];
} i2c_operation_t;

static const char *const uart_quiz_questions
    [BADGE_CTF_UART_QUIZ_ANSWER_COUNT] = {
        "1. What does UART stand for?\r\n"
        "A. Universal Analog Receive Terminal\r\n"
        "B. Universal Asynchronous Receiver/Transmitter\r\n"
        "C. Unified Address Routing Tool\r\n"
        "D. Universal Automatic Response Timer\r\n",
        "2. For a typical direct UART connection between two devices, how "
        "should the data pins connect?\r\n"
        "A. TX to TX and RX to RX\r\n"
        "B. Both TX pins to GND\r\n"
        "C. Both RX pins to VCC\r\n"
        "D. TX to RX and RX to TX\r\n",
        "3. What must normally match between two communicating UART devices?\r\n"
        "A. Baud rate and frame format\r\n"
        "B. Manufacturer\r\n"
        "C. CPU clock frequency\r\n"
        "D. Firmware language\r\n",
        "4. What does the \"8\" in 8N1 mean?\r\n"
        "A. Eight stop bits\r\n"
        "B. Eight bytes per message\r\n"
        "C. Eight parity bits\r\n"
        "D. Eight data bits per frame\r\n",
        "5. What does the \"N\" in 8N1 mean?\r\n"
        "A. Negative polarity\r\n"
        "B. No parity\r\n"
        "C. No stop bit\r\n"
        "D. Network mode\r\n",
        "6. What does the \"1\" in 8N1 mean?\r\n"
        "A. One data bit\r\n"
        "B. One device\r\n"
        "C. One stop bit\r\n"
        "D. One volt\r\n",
        "7. Which device commonly connects a computer's USB port to a "
        "microcontroller's logic-level UART?\r\n"
        "A. A USB power meter\r\n"
        "B. A USB hub\r\n"
        "C. A USB audio adapter\r\n"
        "D. A USB-to-UART adapter\r\n",
        "8. A terminal sends CR followed by LF when Enter is pressed. How many "
        "bytes does it send for that line ending?\r\n"
        "A. Zero\r\n"
        "B. One\r\n"
        "C. Two\r\n"
        "D. Four\r\n",
        "9. How many total bits are transmitted for one byte using 8N1?\r\n"
        "A. Eight\r\n"
        "B. Ten\r\n"
        "C. Nine\r\n"
        "D. Eleven\r\n",
        "10. How long does it take to transmit 100 bytes at 9600 baud using "
        "8N1, with no gaps?\r\n"
        "A. About 10.4 milliseconds\r\n"
        "B. About 1.04 milliseconds\r\n"
        "C. About 1.04 seconds\r\n"
        "D. About 104 milliseconds\r\n",
};

static menu_t menu = MENU_MAIN;
static badge_settings_t settings = {
    25, BADGE_TUNE_STARTUP_JINGLE, LED_ANIMATION_RAINBOW};
static char input_line[192];
static size_t input_length;
static size_t input_cursor;
static escape_state_t escape_state;
static bool skip_lf;
static bool gpio1_flag_shown;
static bool gpio3_output_active;
static uint32_t gpio3_next_output_ms;
static char gpio3_flag[BADGE_CTF_FLAG_TEXT_SIZE];
static uart_console_menu_t uart_console_menu;
static char uart1_line[32];
static size_t uart1_line_length;
static bool uart1_line_overflow;
static bool uart1_skip_lf;
static char uart2_answers[BADGE_CTF_UART_QUIZ_ANSWER_COUNT];
static uint8_t uart2_question;
static uint32_t uart2_retry_after_ms;
static const char *prompt(void) {
    if (menu == MENU_LED) {
        return "led> ";
    }
    if (menu == MENU_TUNES) {
        return "tunes> ";
    }
    if (menu == MENU_SETTINGS_BRIGHTNESS) {
        return "brightness> ";
    }
    if (menu == MENU_SETTINGS_STARTUP) {
        return "startup> ";
    }
    if (menu == MENU_SETTINGS_DEFAULT_LED) {
        return "default-led> ";
    }
    if (menu == MENU_SETTINGS) {
        return "settings> ";
    }
    if (menu == MENU_WELCOME) {
        return "welcome> ";
    }
    if (menu == MENU_GPIO1) {
        return "gpio1> ";
    }
    if (menu == MENU_GPIO2) {
        return "gpio2> ";
    }
    if (menu == MENU_GPIO3) {
        return "gpio3> ";
    }
    if (menu == MENU_UART1) {
        return "uart1> ";
    }
    if (menu == MENU_UART2) {
        return "uart2> ";
    }
    if (menu == MENU_I2C1_CHALLENGE) {
        return "i2c1> ";
    }
    if (menu == MENU_I2C2_CHALLENGE) {
        return "i2c2> ";
    }
    if (menu == MENU_GERBER_CHALLENGE) {
        return "gerber> ";
    }
    if (menu == MENU_SCHEMATIC_CHALLENGE) {
        return "schematic> ";
    }
    if (menu == MENU_UPDI) {
        return badge_updiprog_prompt();
    }
    if (menu == MENU_FILES) {
        return "files> ";
    }
    if (menu == MENU_I2C) {
        return "i2c> ";
    }
    if (menu == MENU_I2C_SCRIPT) {
        return "i2c-script> ";
    }
    if (menu == MENU_NEOPIXEL1) {
        return "neopixel1> ";
    }
    if (menu == MENU_NEOPIXEL2) {
        return "neopixel2> ";
    }
    return menu == MENU_CTF ? "ctf> " : "badge> ";
}

static void print_prompt(void) {
    printf(BADGE_ANSI_RESET BADGE_ANSI_GOLD "[LED: %s] "
           BADGE_ANSI_RESET, badge_leds_name());
    printf(BADGE_ANSI_MAGENTA "[CTF: %u/%u] " BADGE_ANSI_RESET,
           (unsigned)badge_ctf_solved_count(),
           (unsigned)BADGE_CTF_CHALLENGE_COUNT);
    printf(BADGE_ANSI_RESET BADGE_ANSI_BOLD BADGE_ANSI_BLUE "%s"
           BADGE_ANSI_RESET, prompt());
    fflush(stdout);
}

static void redraw_input(void) {
    printf("\r\x1b[2K");
    print_prompt();
    fwrite(input_line, 1, input_length, stdout);
    if (input_cursor < input_length) {
        printf("\x1b[%uD", (unsigned)(input_length - input_cursor));
    }
    fflush(stdout);
}

static void print_menu_title(const char *title) {
    printf("\n" BADGE_ANSI_BOLD BADGE_ANSI_MAGENTA "%s"
           BADGE_ANSI_RESET "\n" BADGE_ANSI_LAVENDER, title);
}

static void print_menu_section(const char *section) {
    printf("\n" BADGE_ANSI_BOLD BADGE_ANSI_GOLD "%s"
           BADGE_ANSI_RESET "\n" BADGE_ANSI_LAVENDER, section);
}

static void print_menu_subsection(const char *subsection) {
    printf("\n" BADGE_ANSI_BOLD BADGE_ANSI_ORANGE "%s"
           BADGE_ANSI_RESET "\n" BADGE_ANSI_LAVENDER, subsection);
}

static void print_welcome_message(void) {
    char welcome_flag[BADGE_CTF_FLAG_TEXT_SIZE] = {0};
    int result = badge_ctf_make_flag(BADGE_FLAG_WELCOME, welcome_flag);

    print_menu_title("Welcome");
    puts("\nAs you now know, the main way to communicate with this badge is\n"
         "via USB CDC using a serial monitor (screen or picocom for Linux;\n"
         "WezTerm or VS Code for Windows). There is a CTF with 12\n"
         "challenges. Some of the challenges WILL require you to\n"
         "disassemble the badge. Please be careful while doing so as to not\n"
         "damage the badge. Please, if you have any questions, come by the\n"
         "Hardware Village and ask!\n");
    puts("There are a couple of extra features, including:\n");
    puts("- UPDI programmer (left SAO port)");
    puts("- Mass Storage device for important files");
    puts("- Basic I2C sniffer\n");
    puts("The above features can be used after the conference.\n");
    puts("Come by the Hardware Village for a Free SAO to complete some of\n"
         "the challenges. Have a good conference and enjoy the badge!\n");
    if (result == PICO_OK) {
        printf("Here's a free flag: %s\n\n", welcome_flag);
    } else {
        puts("Here's a free flag: (unavailable)\n");
    }
    memset(welcome_flag, 0, sizeof(welcome_flag));
}

static void print_main_menu(void) {
    print_welcome_message();
    puts("1) LED");
    puts("2) Tunes");
    puts("3) Settings");
    puts("4) CTF");
    puts("5) UPDI (SAO programmer)");
    puts("6) FILES");
    puts("7) I2C");
    print_prompt();
}

static void print_files_menu(void) {
    print_menu_title("FILES - 3 MiB persistent USB drive");
    badge_files_status_t status = badge_files_prepare();
    if (status == BADGE_FILES_CREATED) {
        puts("Created the empty filesystem.");
    } else if (status == BADGE_FILES_REPAIRED) {
        puts("Repaired the filesystem capacity.");
    } else if (status == BADGE_FILES_UNSUPPORTED) {
        puts("The drive is not FAT12/FAT16, so files cannot be listed here.");
    } else if (status == BADGE_FILES_ERROR) {
        puts("The filesystem could not be prepared.");
    }
    if (status == BADGE_FILES_READY || status == BADGE_FILES_CREATED ||
        status == BADGE_FILES_REPAIRED) {
        badge_files_print_listing();
    }
    if (status != BADGE_FILES_ERROR) {
        puts("1) Enter USB drive mode");
    }
    puts("2) back");
    print_prompt();
}

static void print_i2c_menu(void) {
    print_menu_title("I2C - 400 kHz");
    puts("\nSome basic i2c tools to work with the 2 i2c busses on this\n"
         "device.\n");
    puts("Scan lets you scan either bus for client devices you will need to\n"
         "specify which SAO port as they are on different busses on the\n"
         "MCU.\n");
    puts("Read lets you read from client devices on the bus you need to\n"
         "specify the SAO port, the device address, the register address\n"
         "and the length of data to read (1-32 bytes).\n");
    puts("Write lets you write to client devices on the bus you need to\n"
         "specify the SAO port, the device address, the register address\n"
         "and the data bytes to write (1-32 bytes).\n");
    puts("Script lets you string together a series of commands to run\n"
         "quickly and easily. ** this may or may not be important ;)\n");
    puts("left:  SAO1 / I2C1 / GPIO10 SDA, GPIO11 SCL");
    puts("right: SAO2 / I2C0 / GPIO24 SDA, GPIO25 SCL");
    puts("scan <left|right>");
    puts("read <port> <device> <register> <length 1-32>");
    puts("write <port> <device> <register> <byte> [byte ...]");
    puts("script");
    puts("back");
    puts("Device addresses use 0x-prefixed hex; other values accept decimal or hex.");
    print_prompt();
}

static void print_i2c_script_menu(void) {
    print_menu_title("I2C script");
    puts("Paste read/write operations separated by ';' to run immediately.");
    puts("read <port> <device> <register> <length 1-32>");
    puts("write <port> <device> <register> <byte> [byte ...]");
    puts("back");
    print_prompt();
}

static uint8_t unlocked_led_animation_count(void) {
    uint8_t solved = badge_ctf_solved_count();
    uint8_t challenge_animations =
        LED_ANIMATION_OFF - LED_ANIMATION_SLOW_RAINBOW_ORBIT;
    if (solved > challenge_animations) {
        solved = challenge_animations;
    }
    return LED_ANIMATION_SLOW_RAINBOW_ORBIT + solved;
}

static bool led_animation_unlocked(led_animation_t animation) {
    return animation < unlocked_led_animation_count() ||
           animation == LED_ANIMATION_OFF;
}

static void print_led_menu(void) {
    print_menu_title("LED animations");
    for (led_animation_t animation = 0; animation < LED_ANIMATION_COUNT;
         ++animation) {
        printf("%u) %s%s\n", (unsigned)animation + 1,
               led_animation_unlocked(animation) ? "" : "[LOCKED] ",
               badge_leds_animation_name(animation));
    }
    puts("22) back");
    print_prompt();
}

static bool tune_unlocked(size_t tune_index) {
    if (tune_index == BADGE_TUNE_SIMPSONS) {
        return badge_ctf_gerber_solved();
    }
    if (tune_index == BADGE_TUNE_G_THANG) {
        return badge_ctf_schematic_solved();
    }
    return true;
}

static void print_tunes_menu(void) {
    print_menu_title("Tunes");
    for (size_t i = 0; i < BADGE_TUNE_COUNT; ++i) {
        printf("%u) %s%s\n", (unsigned)i + 1,
               tune_unlocked(i) ? "" : "[LOCKED] ", badge_tunes[i].name);
    }
    printf("%u) back\n", (unsigned)BADGE_TUNE_COUNT + 1);
    print_prompt();
}

static void play_tune_once(size_t tune_index) {
    if (!tune_unlocked(tune_index)) {
        puts("Tune locked. Complete its Hardware Files challenge to unlock it.");
        print_tunes_menu();
        return;
    }
    printf("Playing %s...\n", badge_tunes[tune_index].name);
    fflush(stdout);
    badge_play_tune(badge_tunes[tune_index].tones,
                    badge_tunes[tune_index].tone_count);
    print_tunes_menu();
}

static void print_settings_menu(void) {
    print_menu_title("Settings");
    printf("1) LED brightness (%u%%)\n",
           (unsigned)settings.led_brightness_percent);
    printf("2) startup tune (%s)\n", badge_tunes[settings.startup_tune].name);
    printf("3) default LED animation (%s)\n",
           badge_leds_animation_name(
               (led_animation_t)settings.default_led_animation));
    puts("4) back");
    print_prompt();
}

static void print_brightness_menu(void) {
    print_menu_title("LED brightness");
    for (size_t i = 0; i < BADGE_BRIGHTNESS_CHOICE_COUNT; ++i) {
        printf("%u) [%c] %u%%\n", (unsigned)i + 1,
               settings.led_brightness_percent == badge_brightness_choices[i]
                   ? 'x' : ' ',
               (unsigned)badge_brightness_choices[i]);
    }
    printf("%u) back\n", (unsigned)BADGE_BRIGHTNESS_CHOICE_COUNT + 1);
    print_prompt();
}

static void print_startup_tune_menu(void) {
    print_menu_title("Startup tune");
    for (size_t i = 0; i < BADGE_TUNE_COUNT; ++i) {
        printf("%u) [%c] %s%s\n", (unsigned)i + 1,
               settings.startup_tune == i ? 'x' : ' ',
               tune_unlocked(i) ? "" : "[LOCKED] ", badge_tunes[i].name);
    }
    printf("%u) back\n", (unsigned)BADGE_TUNE_COUNT + 1);
    print_prompt();
}

static void print_default_led_animation_menu(void) {
    print_menu_title("Default LED animation");
    for (led_animation_t animation = 0; animation < LED_ANIMATION_COUNT;
         ++animation) {
        printf("%u) [%c] %s%s\n", (unsigned)animation + 1,
               settings.default_led_animation == animation ? 'x' : ' ',
               led_animation_unlocked(animation) ? "" : "[LOCKED] ",
               badge_leds_animation_name(animation));
    }
    printf("%u) back\n", (unsigned)LED_ANIMATION_COUNT + 1);
    print_prompt();
}

static void print_gpio1_challenge(void) {
    print_menu_title("GPIO1: Bridge");
    puts("\nThe golden gate is wonderful but its got nothing on the GPIO12toGND");
    puts("Enter a flag, or 'back' to return.");
    print_prompt();
}

static void print_welcome_challenge(void) {
    print_welcome_message();
    puts("Enter a flag, or 'back' to return.");
    print_prompt();
}

static void print_neopixel1_challenge(void) {
    print_menu_title("NEOPIXEL1");
    puts("\nOne of the led animations is uhm weird. See if you can decode why!");
    puts("Enter a flag, or 'back' to return.");
    print_prompt();
}

static void print_neopixel2_challenge(void) {
    print_menu_title("NEOPIXEL2");
    puts("\nSilly me! I forgot to read the datasheet! Will you take a look and see what order we should send the color data? Maybe that will help decode why the colors are off?");
    puts("Enter a flag, or 'back' to return.");
    print_prompt();
}

static void reveal_gpio1_flag_if_grounded(void) {
    if (menu != MENU_GPIO1 || gpio1_flag_shown ||
        !badge_gpio12_grounded()) {
        return;
    }

    char flag[BADGE_CTF_FLAG_TEXT_SIZE] = {0};
    int result = badge_ctf_make_flag(BADGE_FLAG_GPIO1, flag);
    if (result == PICO_OK) {
        printf("\n%s\n", flag);
    } else {
        printf("\nGPIO1 flag unavailable (%d).\n", result);
    }
    memset(flag, 0, sizeof(flag));
    gpio1_flag_shown = true;
    redraw_input();
}

static void print_gpio2_challenge(void) {
    uint8_t levels_solved = badge_ctf_gpio_levels_solved();
    print_menu_title("GPIO2: Microsoldering");
    puts("\nSo you wanna learn to microsolder huh? who hurt you? ..... I mean if you insist heres a challenge to help you get started");
    puts("ive broken out 4 gpio pins each one is has  the option to be bridged HIGH(3.3v) or LOW(GND)");
    puts("each round you will get a combination that will look something like this 19[]20[]21[]22[] the number inicates the GPIO pin number");
    puts("the brackets will be replaced with either an H for HIGH L for LOW or F for floating which is to say dont bridge either or. Your challenge is to turn the badge off from power and complete the challenge then restart badge if the level you are currently on was soldered correctly check below to see the status of the solve.");
    puts("!!!!IMPORTANT NOTE!!!!");
    puts("#1 This challenge does not require a lot of solder if you find your self having to apply a ton of solder to your iron then something else could be causing the solder not to stick properly");
    puts("#2 flux will help remove oxidation from the solder pads and allow solder to flow MUCH easier to these pads.");
    puts("#3 DONT be afraid to break out some solderwick to help with this challenge as it will help remove any misplaced solder allow you to go back in and get a cleaner bridge");
    printf("\nlevel 1 [%s] 19[H]20[F]21[F]22[F]\n",
           levels_solved >= 1 ? "SOLVED" : " ");
    printf("level 2 [%s] 19[L]20[F]21[F]22[H]\n",
           levels_solved >= 2 ? "SOLVED" : " ");
    printf("level 3 [%s] 19[F]20[H]21[L]22[L]\n",
           levels_solved >= 3 ? "SOLVED" : " ");
    printf("level 4 [%s] 19[H]20[L]21[H]22[l]\n",
           levels_solved >= 4 ? "SOLVED" : " ");

    if (levels_solved == BADGE_CTF_GPIO_LEVEL_COUNT) {
        char flag[BADGE_CTF_FLAG_TEXT_SIZE] = {0};
        int result = badge_ctf_make_flag(BADGE_FLAG_GPIO2, flag);
        if (result == PICO_OK) {
            printf("\n%s\n", flag);
        } else {
            printf("\nGPIO2 flag unavailable (%d).\n", result);
        }
        memset(flag, 0, sizeof(flag));
    }
    puts(levels_solved == BADGE_CTF_GPIO_LEVEL_COUNT
             ? "\nEnter the flag above, or 'back' to return."
             : "\nEnter 'back' to return.");
    print_prompt();
}

static void print_gpio3_challenge(void) {
    print_menu_title("GPIO3: Analyze");
    puts("\nLife's full of highs and lows my friend GPIO12 is the best at making the most of them! See if you can ANALYZE it to see whats going on!");
    puts("\nDisconnect GPIO12 from GND before viewing this challenge.");
    puts("Enter a flag, or 'back' to return.");
    print_prompt();
}

static void print_uart1_challenge(void) {
    print_menu_title("UART1");
    puts("\nIts nice to hear you say hello");
    puts("Enter a flag, or 'back' to return.");
    print_prompt();
}

static void print_uart2_challenge(void) {
    print_menu_title("UART2: Quiz");
    puts("\nComplete the quiz on the hardware UART console.");
    puts("Enter a flag, or 'back' to return.");
    print_prompt();
}

static void print_i2c1_challenge(void) {
    print_menu_title("I2C1");
    puts("\ni too see the flag");
    puts("Enter a flag, or 'back' to return.");
    print_prompt();
}

static void print_i2c2_challenge(void) {
    print_menu_title("I2C2");
    puts("\nfor every action there is a reaction");
    puts("HINT you will need to read the SAO src(in files) for this one");
    puts("Enter a flag, or 'back' to return.");
    print_prompt();
}

static void print_gerber_challenge(void) {
    print_menu_title("Gerber file");
    puts("\nyou may be able to get this one without the gerber file haha!\n"
         "#freeflag");
    puts("Enter a flag, or 'back' to return.");
    print_prompt();
}

static void print_schematic_challenge(void) {
    print_menu_title("Schematic flag");
    puts("\nThe schematic is the base for ALL circuits and is crucial to\n"
         "finding flags");
    puts("Enter a flag, or 'back' to return.");
    print_prompt();
}

static void stop_gpio3_output(void) {
    if (!gpio3_output_active) {
        return;
    }
    badge_gpio12_uart_stop();
    memset(gpio3_flag, 0, sizeof(gpio3_flag));
    gpio3_output_active = false;
}

static void service_gpio3_output(bool usb_connected, uint32_t now_ms) {
    if (!usb_connected || menu != MENU_GPIO3) {
        stop_gpio3_output();
        return;
    }
    if (!gpio3_output_active) {
        if (badge_gpio12_grounded() ||
            badge_ctf_make_flag(BADGE_FLAG_GPIO3, gpio3_flag) != PICO_OK) {
            return;
        }
        badge_gpio12_uart_start();
        gpio3_output_active = true;
        gpio3_next_output_ms = now_ms;
    }
    if ((int32_t)(now_ms - gpio3_next_output_ms) >= 0) {
        badge_gpio12_uart_write(gpio3_flag, strlen(gpio3_flag));
        gpio3_next_output_ms = now_ms + 1000;
    }
}

static void print_ctf_menu(void) {
    print_menu_title("CTF challenges");
    print_menu_section("MISC");
    printf("  1) [%s] Welcome\n",
           badge_ctf_welcome_solved() ? "SOLVED" : " ");
    print_menu_section("LEDS");
    puts("NeoPixels are individually addressable RGB or RGBW LEDs controlled\n"
         "through a single data wire. Each LED contains a small controller\n"
         "that receives a stream of digital color data. Rather than using\n"
         "addressed packets like I²C, the position of each color value in the\n"
         "stream determines which LED receives it. The first color value is\n"
         "accepted by the first LED, the second value travels to the second\n"
         "LED, and so on. Each LED removes its data from the stream and\n"
         "forwards the remaining data through its DOUT pin. Power, ground, and\n"
         "data are normally the only connections required. This allows a large\n"
         "chain of LEDs to be controlled using one microcontroller pin.");
    printf("  2) [%s] NEOPIXEL1\n",
           badge_ctf_neopixel1_solved() ? "SOLVED" : " ");
    printf("  3) [%s] NEOPIXEL2\n",
           badge_ctf_neopixel2_solved() ? "SOLVED" : " ");
    print_menu_section("GPIO");
    puts("GPIO stands for General-Purpose Input/Output. GPIO pins are\n"
         "programmable pins on a microcontroller, processor, or development\n"
         "board. Each pin can usually be configured as either an input or an\n"
         "output. As an input, the pin reads whether an external signal is\n"
         "logically high or low. As an output, the pin drives a high or low\n"
         "voltage. GPIO is commonly used with buttons, LEDs, relays, sensors,\n"
         "and other simple hardware. Unlike UART or I²C, GPIO is not a\n"
         "communication protocol by itself.");
    printf("  4) [%s] GPIO1: Bridge\n",
           badge_ctf_gpio1_solved() ? "SOLVED" : " ");
    printf("  5) [%s] GPIO2: Microsoldering\n",
           badge_ctf_gpio2_solved() ? "SOLVED" : " ");
    printf("  6) [%s] GPIO3: Analyze\n",
           badge_ctf_gpio3_solved() ? "SOLVED" : " ");
    print_menu_section("Embedded protocols");
    print_menu_subsection("UART");
    puts("UART stands for Universal Asynchronous Receiver/Transmitter. It\n"
         "allows two devices to exchange data using a transmit line called TX\n"
         "and a receive line called RX. The TX pin of one device connects to\n"
         "the RX pin of the other, and vice versa. Because UART is\n"
         "asynchronous, it does not use a separate clock wire. Instead, both\n"
         "devices agree on a baud rate, such as 9,600 or 115,200 bits per\n"
         "second. Each byte is packaged into a frame containing a start bit,\n"
         "data bits, and one or more stop bits. Both devices must use matching\n"
         "settings for communication to work reliably.");
    printf("  7) [%s] UART1\n",
           badge_ctf_uart1_solved() ? "SOLVED" : " ");
    printf("  8) [%s] UART2: Quiz\n",
           badge_ctf_uart2_solved() ? "SOLVED" : " ");
    print_menu_subsection("I2C");
    puts("I²C stands for Inter-Integrated Circuit and allows multiple devices\n"
         "to communicate over the same two signal wires. The two wires are SDA\n"
         "for data and SCL for the clock. A controller starts communication\n"
         "and generates the clock signal, while target devices respond when\n"
         "addressed. Every target normally has a 7-bit address that identifies\n"
         "it on the bus. The controller sends an address along with a bit\n"
         "indicating whether it wants to read or write. The addressed target\n"
         "acknowledges the request before data is transferred. This design\n"
         "allows several sensors, displays, and other chips to share the same\n"
         "connection.");
    printf("  9) [%s] I2C1\n",
           badge_ctf_i2c1_solved() ? "SOLVED" : " ");
    printf(" 10) [%s] I2C2\n",
           badge_ctf_i2c2_solved() ? "SOLVED" : " ");
    print_menu_section("Hardware Files");
    puts("Hardware design files describe how an electronic circuit board should\n"
         "work and how it should be manufactured. A project commonly contains\n"
         "a schematic and a Gerber file. The schematic describes the\n"
         "components and the electrical connections between them. It helps\n"
         "someone understand how the circuit operates and how each component\n"
         "is connected. The Gerber file contains the manufacturing information\n"
         "needed to produce the physical circuit board. It normally includes\n"
         "the copper, solder mask, silkscreen, board outline, and drilling\n"
         "information. Together, the schematic and Gerber files take a project\n"
         "from an electrical design to a physical circuit board. I've placed\n"
         "these files in the file system of this badge.");
    printf(" 11) [%s] Gerber file\n",
           badge_ctf_gerber_solved() ? "SOLVED" : " ");
    printf(" 12) [%s] Schematic flag\n",
           badge_ctf_schematic_solved() ? "SOLVED" : " ");
    puts("\nSelect 1-12 to view a challenge description.");
    puts("Enter a flag, or 'back' to return.");
    print_prompt();
}

static void report_settings_save(void) {
    puts(badge_settings_save(&settings)
             ? "Setting saved."
             : "Setting applied, but could not be saved.");
}

static void lowercase_navigation_command(char *command) {
    for (; *command; ++command) {
        *command = (char)tolower((unsigned char)*command);
    }
}

static bool navigation_command_is(const char *command, const char *expected) {
    while (*command && *expected &&
           tolower((unsigned char)*command) == (unsigned char)*expected) {
        ++command;
        ++expected;
    }
    return *command == '\0' && *expected == '\0';
}

static bool parse_i2c_number(const char *text, uint32_t maximum,
                             uint32_t *value) {
    if (!text || !text[0]) {
        return false;
    }
    int base = text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 16 : 10;
    char *end;
    unsigned long parsed = strtoul(text, &end, base);
    if (*end || parsed > maximum) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_i2c_port(const char *text, badge_i2c_port_t *port) {
    if (text && (!strcmp(text, "left") || !strcmp(text, "1"))) {
        *port = BADGE_I2C_PORT_LEFT;
        return true;
    }
    if (text && (!strcmp(text, "right") || !strcmp(text, "2"))) {
        *port = BADGE_I2C_PORT_RIGHT;
        return true;
    }
    return false;
}

static const char *i2c_port_name(badge_i2c_port_t port) {
    return port == BADGE_I2C_PORT_LEFT ? "left" : "right";
}

static bool parse_i2c_operation(char *command, i2c_operation_t *operation) {
    char *kind = strtok(command, " \t");
    char *port_text = strtok(NULL, " \t");
    char *address_text = strtok(NULL, " \t");
    char *register_text = strtok(NULL, " \t");
    badge_i2c_port_t port;
    uint32_t address;
    uint32_t reg;

    if (!kind || !parse_i2c_port(port_text, &port) ||
        !parse_i2c_number(address_text, 0x7f, &address) ||
        address < 0x08 || address > 0x77 ||
        !parse_i2c_number(register_text, 0xff, &reg)) {
        return false;
    }

    memset(operation, 0, sizeof(*operation));
    operation->port = port;
    operation->address = (uint8_t)address;
    operation->reg = (uint8_t)reg;
    if (!strcmp(kind, "read")) {
        char *length_text = strtok(NULL, " \t");
        uint32_t length;
        if (!parse_i2c_number(length_text, BADGE_I2C_MAX_TRANSFER, &length) ||
            !length || strtok(NULL, " \t")) {
            return false;
        }
        operation->kind = I2C_OPERATION_READ;
        operation->length = (uint8_t)length;
        return true;
    }
    if (strcmp(kind, "write")) {
        return false;
    }

    operation->kind = I2C_OPERATION_WRITE;
    char *byte_text;
    while ((byte_text = strtok(NULL, " \t"))) {
        uint32_t byte;
        if (operation->length == BADGE_I2C_MAX_TRANSFER ||
            !parse_i2c_number(byte_text, 0xff, &byte)) {
            return false;
        }
        operation->data[operation->length++] = (uint8_t)byte;
    }
    return operation->length != 0;
}

static void print_i2c_dump(uint8_t reg, const uint8_t *data, size_t length) {
    enum { BYTES_PER_ROW = 5 };

    puts(BADGE_ANSI_BOLD BADGE_ANSI_GOLD "ADDR  HEX              TEXT"
         BADGE_ANSI_RESET BADGE_ANSI_LAVENDER);
    for (size_t offset = 0; offset < length; offset += BYTES_PER_ROW) {
        size_t row_length = length - offset;
        if (row_length > BYTES_PER_ROW) {
            row_length = BYTES_PER_ROW;
        }

        printf("0x%02x  ", (uint8_t)(reg + offset));
        for (size_t i = 0; i < BYTES_PER_ROW; ++i) {
            if (i < row_length) {
                printf("%02x ", data[offset + i]);
            } else {
                printf("   ");
            }
        }
        printf(" ");
        for (size_t i = 0; i < row_length; ++i) {
            uint8_t byte = data[offset + i];
            putchar(isprint(byte) ? byte : '.');
        }
        putchar('\n');
    }
}

static void execute_i2c_operation(const i2c_operation_t *operation) {
    if (operation->kind == I2C_OPERATION_READ) {
        uint8_t data[BADGE_I2C_MAX_TRANSFER] = {0};
        int result = badge_i2c_read_register(
            operation->port, operation->address, operation->reg, data,
            operation->length);
        if (result != operation->length) {
            printf("read failed (%d)\n", result);
            return;
        }
        print_i2c_dump(operation->reg, data, operation->length);
        return;
    }

    int result = badge_i2c_write_register(
        operation->port, operation->address, operation->reg, operation->data,
        operation->length);
    if (result == operation->length + 1) {
        puts("OK");
    } else {
        printf("write failed (%d)\n", result);
    }
}

static bool execute_i2c_line(char *command) {
    i2c_operation_t operations[I2C_LINE_MAX_OPERATIONS];
    size_t operation_count = 0;

    while (command) {
        char *next = strchr(command, ';');
        if (next) {
            *next++ = '\0';
        }
        while (isspace((unsigned char)*command)) {
            ++command;
        }
        char *end = command + strlen(command);
        while (end > command && isspace((unsigned char)end[-1])) {
            *--end = '\0';
        }
        if (!*command || operation_count == I2C_LINE_MAX_OPERATIONS ||
            !parse_i2c_operation(command, &operations[operation_count])) {
            return false;
        }
        ++operation_count;
        command = next;
    }

    for (size_t i = 0; i < operation_count; ++i) {
        if (operation_count > 1) {
            printf("[%u] ", (unsigned)i + 1);
        }
        execute_i2c_operation(&operations[i]);
    }
    return true;
}

static void print_i2c_usage(void) {
    puts("Use: read <port> <device> <register> <length 1-32>");
    puts(" or: write <port> <device> <register> <byte> [byte ...]");
    puts("Separate operations with ';' to run them in sequence.");
    puts("Device addresses must be 7-bit values from 0x08 through 0x77.");
}

static void scan_i2c_port(badge_i2c_port_t port) {
    size_t found = 0;
    printf("Scanning %s port...\n", i2c_port_name(port));
    for (uint8_t address = 0x08; address <= 0x77; ++address) {
        if (badge_i2c_probe(port, address) == 1) {
            if (!found) {
                printf("Found:");
            }
            printf(" 0x%02x", address);
            ++found;
        }
    }
    if (found) {
        printf("\n%u device%s found.\n", (unsigned)found,
               found == 1 ? "" : "s");
    } else {
        puts("No devices found.");
    }
}

static void handle_i2c_command(char *command) {
    if (navigation_command_is(command, "back")) {
        menu = MENU_MAIN;
        print_main_menu();
    } else if (navigation_command_is(command, "script")) {
        menu = MENU_I2C_SCRIPT;
        print_i2c_script_menu();
    } else if (!strncmp(command, "scan", 4) &&
               (!command[4] || isspace((unsigned char)command[4]))) {
        strtok(command, " \t");
        char *port_text = strtok(NULL, " \t");
        badge_i2c_port_t port;
        if (parse_i2c_port(port_text, &port) && !strtok(NULL, " \t")) {
            scan_i2c_port(port);
        } else {
            puts("Use: scan <left|right>");
        }
        print_prompt();
    } else {
        if (!execute_i2c_line(command)) {
            print_i2c_usage();
        }
        print_prompt();
    }
}

static void handle_i2c_script_command(char *command) {
    if (navigation_command_is(command, "back")) {
        menu = MENU_I2C;
        print_i2c_menu();
        return;
    }
    if (!execute_i2c_line(command)) {
        print_i2c_usage();
    }
    print_prompt();
}

static void uart1_write_text(const char *text) {
    badge_uart1_write(text, strlen(text));
}

static void print_uart_console_prompt(void) {
    uart1_write_text(BADGE_ANSI_RESET BADGE_ANSI_BOLD BADGE_ANSI_BLUE);
    if (uart_console_menu == UART_CONSOLE_CHALLENGE_1) {
        uart1_write_text("uart1> ");
    } else if (uart_console_menu == UART_CONSOLE_CHALLENGE_2) {
        uart1_write_text("uart2> ");
    } else {
        uart1_write_text("uart> ");
    }
    uart1_write_text(BADGE_ANSI_RESET);
}

static void print_uart_console_menu(void) {
    uart_console_menu = UART_CONSOLE_MENU;
    uart1_write_text(
        BADGE_ANSI_BOLD BADGE_ANSI_MAGENTA "UART challenges:"
        BADGE_ANSI_RESET "\r\n" BADGE_ANSI_LAVENDER
        "  1) UART challenge 1\r\n"
        "  2) UART challenge 2\r\n"
        "Choose 1 or 2.\r\n");
    print_uart_console_prompt();
}

static void print_uart_console_challenge_1(void) {
    uart_console_menu = UART_CONSOLE_CHALLENGE_1;
    uart1_write_text(
        BADGE_ANSI_BOLD BADGE_ANSI_MAGENTA "UART challenge 1"
        BADGE_ANSI_RESET "\r\n" BADGE_ANSI_LAVENDER
        "Its nice to hear you say hello\r\n");
    print_uart_console_prompt();
}

static void reset_uart_quiz(void) {
    memset(uart2_answers, 0, sizeof(uart2_answers));
    uart2_question = 0;
    uart2_retry_after_ms = 0;
}

static void print_uart_quiz_question(void) {
    uart1_write_text(BADGE_ANSI_LAVENDER);
    uart1_write_text(uart_quiz_questions[uart2_question]);
    uart1_write_text(BADGE_ANSI_BOLD BADGE_ANSI_BLUE "Answer> "
                     BADGE_ANSI_RESET);
}

static void print_uart_console_challenge_2_current(void) {
    uart1_write_text(BADGE_ANSI_BOLD BADGE_ANSI_MAGENTA "UART challenge 2"
                     BADGE_ANSI_RESET "\r\n" BADGE_ANSI_LAVENDER);
    if (!badge_ctf_uart2_available()) {
        uart1_write_text("Challenge unavailable.\r\n");
        print_uart_console_prompt();
    } else if (uart2_retry_after_ms) {
        print_uart_console_prompt();
    } else {
        print_uart_quiz_question();
    }
}

static void print_uart_console_challenge_2(void) {
    uart_console_menu = UART_CONSOLE_CHALLENGE_2;
    reset_uart_quiz();
    print_uart_console_challenge_2_current();
}

static void print_uart_console_current(void) {
    if (uart_console_menu == UART_CONSOLE_CHALLENGE_1) {
        print_uart_console_challenge_1();
    } else if (uart_console_menu == UART_CONSOLE_CHALLENGE_2) {
        print_uart_console_challenge_2_current();
    } else {
        print_uart_console_menu();
    }
}

static void handle_uart_quiz_answer(const char *input) {
    if (!badge_ctf_uart2_available()) {
        print_uart_console_challenge_2_current();
        return;
    }
    if (uart2_retry_after_ms) {
        print_uart_console_prompt();
        return;
    }

    char answer = (char)toupper((unsigned char)input[0]);
    if (input[1] || answer < 'A' || answer > 'D') {
        uart1_write_text("Enter A, B, C, or D.\r\n");
        print_uart_quiz_question();
        return;
    }

    uart2_answers[uart2_question++] = answer;
    if (uart2_question < BADGE_CTF_UART_QUIZ_ANSWER_COUNT) {
        print_uart_quiz_question();
        return;
    }

    badge_uart_quiz_result_t result =
        badge_ctf_submit_uart2_answers(uart2_answers);
    reset_uart_quiz();
    if (result == BADGE_UART_QUIZ_CORRECT) {
        char flag[BADGE_CTF_FLAG_TEXT_SIZE] = {0};
        uart1_write_text("Answer set accepted.\r\n");
        if (badge_ctf_make_flag(BADGE_FLAG_UART2, flag) == PICO_OK) {
            badge_uart1_write(flag, strlen(flag));
            uart1_write_text("\r\n");
        } else {
            uart1_write_text("Flag unavailable.\r\n");
        }
        memset(flag, 0, sizeof(flag));
        print_uart_console_prompt();
    } else if (result == BADGE_UART_QUIZ_INCORRECT) {
        uart1_write_text("The complete answer set is incorrect.\r\n");
        uart2_retry_after_ms =
            to_ms_since_boot(get_absolute_time()) + UART_QUIZ_RETRY_DELAY_MS;
    } else {
        uart1_write_text("Challenge unavailable.\r\n");
        print_uart_console_prompt();
    }
}

static void service_uart_quiz_retry(uint32_t now_ms) {
    if (uart_console_menu == UART_CONSOLE_CHALLENGE_2 &&
        uart2_retry_after_ms &&
        (int32_t)(now_ms - uart2_retry_after_ms) >= 0) {
        uart2_retry_after_ms = 0;
        print_uart_quiz_question();
    }
}

static void handle_uart1_line(void) {
    if (uart1_line_overflow) {
        uart1_write_text("Input is too long.\r\n");
        print_uart_console_current();
        return;
    }

    if (!uart1_line[0]) {
        print_uart_console_current();
        return;
    }

    if (uart_console_menu == UART_CONSOLE_MENU) {
        if (navigation_command_is(uart1_line, "1") ||
            navigation_command_is(uart1_line, "uart1")) {
            print_uart_console_challenge_1();
        } else if (navigation_command_is(uart1_line, "2") ||
                   navigation_command_is(uart1_line, "uart2")) {
            print_uart_console_challenge_2();
        } else if (navigation_command_is(uart1_line, "menu") ||
                   navigation_command_is(uart1_line, "help")) {
            print_uart_console_menu();
        } else {
            print_uart_console_menu();
        }
        return;
    }

    if (navigation_command_is(uart1_line, "back") ||
        navigation_command_is(uart1_line, "menu")) {
        reset_uart_quiz();
        print_uart_console_menu();
        return;
    }

    if (uart_console_menu == UART_CONSOLE_CHALLENGE_2) {
        handle_uart_quiz_answer(uart1_line);
        return;
    }

    if (uart_console_menu == UART_CONSOLE_CHALLENGE_1 &&
        navigation_command_is(uart1_line, "hello")) {
        char flag[BADGE_CTF_FLAG_TEXT_SIZE] = {0};
        if (badge_ctf_make_flag(BADGE_FLAG_UART1, flag) == PICO_OK) {
            badge_uart1_write(flag, strlen(flag));
            uart1_write_text("\r\n");
        } else {
            uart1_write_text("Flag unavailable\r\n");
        }
        memset(flag, 0, sizeof(flag));
        print_uart_console_prompt();
        return;
    }

    print_uart_console_current();
}

static void service_uart1(void) {
    while (badge_uart1_readable()) {
        char character = badge_uart1_read();
        if (character == '\n' && uart1_skip_lf) {
            uart1_skip_lf = false;
            continue;
        }
        if (character == '\r' || character == '\n') {
            uart1_skip_lf = character == '\r';
            uart1_write_text("\r\n");
            uart1_line[uart1_line_length] = '\0';
            handle_uart1_line();
            uart1_line_length = 0;
            uart1_line_overflow = false;
        } else if ((character == '\b' || character == 0x7f) &&
                   uart1_line_length && !uart1_line_overflow) {
            uart1_skip_lf = false;
            --uart1_line_length;
            uart1_write_text("\b \b");
        } else if (character >= 32 && character <= 126) {
            uart1_skip_lf = false;
            badge_uart1_write(&character, 1);
            if (uart1_line_length < sizeof(uart1_line) - 1) {
                uart1_line[uart1_line_length++] = character;
            } else {
                uart1_line_overflow = true;
            }
        }
    }
}

static void submit_challenge_flag(const char *submitted_flag) {
    if (badge_ctf_submit_flag(submitted_flag)) {
        badge_play_tune(badge_tunes[BADGE_TUNE_ARPEGGIO].tones,
                        badge_tunes[BADGE_TUNE_ARPEGGIO].tone_count);
    }
}

static void handle_flag_challenge(const char *command,
                                  void (*print_challenge)(void)) {
    if (navigation_command_is(command, "back")) {
        menu = MENU_CTF;
        print_ctf_menu();
    } else {
        submit_challenge_flag(command);
        print_challenge();
    }
}

static void handle_command(char *command) {
    if (menu == MENU_MAIN) {
        if (!strcmp(command, "1") || !strcmp(command, "led")) {
            menu = MENU_LED;
            print_led_menu();
        } else if (!strcmp(command, "2") || !strcmp(command, "tunes") ||
                   !strcmp(command, "tune")) {
            menu = MENU_TUNES;
            print_tunes_menu();
        } else if (!strcmp(command, "3") || !strcmp(command, "settings") ||
                   !strcmp(command, "setting")) {
            menu = MENU_SETTINGS;
            print_settings_menu();
        } else if (!strcmp(command, "4") || !strcmp(command, "ctf")) {
            menu = MENU_CTF;
            print_ctf_menu();
        } else if (!strcmp(command, "5") || !strcmp(command, "updi")) {
            menu = MENU_UPDI;
            badge_updiprog_enter();
        } else if (!strcmp(command, "6") || !strcmp(command, "files")) {
            menu = MENU_FILES;
            print_files_menu();
        } else if (!strcmp(command, "7") || !strcmp(command, "i2c")) {
            menu = MENU_I2C;
            print_i2c_menu();
        } else {
            puts("Choose 'led', 'tunes', 'settings', 'ctf', 'updi', 'files', or 'i2c'.");
            print_prompt();
        }
        return;
    }

    if (menu == MENU_UPDI) {
        if (!badge_updiprog_handle_command(command)) {
            menu = MENU_MAIN;
            print_main_menu();
        }
        return;
    }

    if (menu == MENU_FILES) {
        if (!strcmp(command, "1") || !strcmp(command, "usb") ||
            !strcmp(command, "drive")) {
            puts("Switching to USB drive mode. The serial console will disconnect.");
            puts("Use your PC's Eject action when finished; the console will return.");
            fflush(stdout);
            if (!badge_files_enter_usb_mode()) {
                puts("USB drive mode is unavailable.");
                print_prompt();
            }
        } else if (!strcmp(command, "2") || !strcmp(command, "back")) {
            menu = MENU_MAIN;
            print_main_menu();
        } else {
            puts("Choose 'usb' or 'back'.");
            print_prompt();
        }
        return;
    }

    if (menu == MENU_I2C) {
        handle_i2c_command(command);
        return;
    }

    if (menu == MENU_I2C_SCRIPT) {
        handle_i2c_script_command(command);
        return;
    }

    if (menu == MENU_TUNES) {
        int tune_index = badge_tune_index_from_command(command);
        if (tune_index >= 0) {
            play_tune_once((size_t)tune_index);
        } else if (!strcmp(command, "8") || !strcmp(command, "back")) {
            menu = MENU_MAIN;
            print_main_menu();
        } else {
            puts("Choose a tune from 1-7, or 'back'.");
            print_prompt();
        }
        return;
    }

    if (menu == MENU_SETTINGS) {
        if (!strcmp(command, "1") || !strcmp(command, "brightness")) {
            menu = MENU_SETTINGS_BRIGHTNESS;
            print_brightness_menu();
        } else if (!strcmp(command, "2") || !strcmp(command, "startup") ||
                   !strcmp(command, "startup-tune")) {
            menu = MENU_SETTINGS_STARTUP;
            print_startup_tune_menu();
        } else if (!strcmp(command, "3") || !strcmp(command, "default") ||
                   !strcmp(command, "default-led")) {
            menu = MENU_SETTINGS_DEFAULT_LED;
            print_default_led_animation_menu();
        } else if (!strcmp(command, "4") || !strcmp(command, "back")) {
            menu = MENU_MAIN;
            print_main_menu();
        } else {
            puts("Choose 'brightness', 'startup', 'default-led', or 'back'.");
            print_prompt();
        }
        return;
    }

    if (menu == MENU_SETTINGS_BRIGHTNESS) {
        if (!strcmp(command, "5") || !strcmp(command, "back")) {
            menu = MENU_SETTINGS;
            print_settings_menu();
        } else if (command[1] == '\0' && command[0] >= '1' && command[0] <= '4') {
            settings.led_brightness_percent =
                badge_brightness_choices[command[0] - '1'];
            badge_leds_set_brightness(settings.led_brightness_percent);
            printf("LED brightness set to %u%%.\n",
                   (unsigned)settings.led_brightness_percent);
            report_settings_save();
            menu = MENU_SETTINGS;
            print_settings_menu();
        } else {
            puts("Choose a brightness from 1-4, or 'back'.");
            print_prompt();
        }
        return;
    }

    if (menu == MENU_SETTINGS_STARTUP) {
        int tune_index = badge_tune_index_from_command(command);
        if (tune_index >= 0 && tune_unlocked((size_t)tune_index)) {
            settings.startup_tune = (uint8_t)tune_index;
            printf("Startup tune set to %s.\n", badge_tunes[tune_index].name);
            report_settings_save();
            menu = MENU_SETTINGS;
            print_settings_menu();
        } else if (tune_index >= 0) {
            puts("Tune locked. Complete its Hardware Files challenge to unlock it.");
            print_prompt();
        } else if (!strcmp(command, "8") || !strcmp(command, "back")) {
            menu = MENU_SETTINGS;
            print_settings_menu();
        } else {
            puts("Choose a startup tune from 1-7, or 'back'.");
            print_prompt();
        }
        return;
    }

    if (menu == MENU_SETTINGS_DEFAULT_LED) {
        if (!strcmp(command, "22") || !strcmp(command, "back")) {
            menu = MENU_SETTINGS;
            print_settings_menu();
            return;
        }

        char *end;
        unsigned long choice = strtoul(command, &end, 10);
        if (!command[0] || *end || choice < 1 ||
            choice > LED_ANIMATION_COUNT) {
            puts("Choose a default animation from 1-21, or 'back'.");
            print_prompt();
            return;
        }

        led_animation_t selected = (led_animation_t)(choice - 1);
        if (!led_animation_unlocked(selected)) {
            puts("Animation locked. Complete another CTF challenge to unlock it.");
            print_prompt();
            return;
        }

        settings.default_led_animation = (uint8_t)selected;
        printf("Default LED animation set to %s; it will apply after restart.\n",
               badge_leds_animation_name(selected));
        report_settings_save();
        menu = MENU_SETTINGS;
        print_settings_menu();
        return;
    }

    if (menu == MENU_NEOPIXEL1) {
        handle_flag_challenge(command, print_neopixel1_challenge);
        return;
    }

    if (menu == MENU_WELCOME) {
        handle_flag_challenge(command, print_welcome_challenge);
        return;
    }

    if (menu == MENU_NEOPIXEL2) {
        handle_flag_challenge(command, print_neopixel2_challenge);
        return;
    }

    if (menu == MENU_GPIO1) {
        handle_flag_challenge(command, print_gpio1_challenge);
        return;
    }

    if (menu == MENU_GPIO2) {
        handle_flag_challenge(command, print_gpio2_challenge);
        return;
    }

    if (menu == MENU_GPIO3) {
        if (navigation_command_is(command, "back")) {
            stop_gpio3_output();
            menu = MENU_CTF;
            print_ctf_menu();
        } else {
            submit_challenge_flag(command);
            print_gpio3_challenge();
        }
        return;
    }

    if (menu == MENU_UART1) {
        handle_flag_challenge(command, print_uart1_challenge);
        return;
    }

    if (menu == MENU_UART2) {
        handle_flag_challenge(command, print_uart2_challenge);
        return;
    }

    if (menu == MENU_I2C1_CHALLENGE) {
        handle_flag_challenge(command, print_i2c1_challenge);
        return;
    }

    if (menu == MENU_I2C2_CHALLENGE) {
        handle_flag_challenge(command, print_i2c2_challenge);
        return;
    }

    if (menu == MENU_GERBER_CHALLENGE) {
        handle_flag_challenge(command, print_gerber_challenge);
        return;
    }

    if (menu == MENU_SCHEMATIC_CHALLENGE) {
        handle_flag_challenge(command, print_schematic_challenge);
        return;
    }

    if (menu == MENU_CTF) {
        if (navigation_command_is(command, "back")) {
            menu = MENU_MAIN;
            print_main_menu();
        } else if (navigation_command_is(command, "1") ||
                   navigation_command_is(command, "welcome")) {
            menu = MENU_WELCOME;
            print_welcome_challenge();
        } else if (navigation_command_is(command, "2") ||
                   navigation_command_is(command, "neopixel1") ||
                   navigation_command_is(command, "neopixel")) {
            menu = MENU_NEOPIXEL1;
            print_neopixel1_challenge();
        } else if (navigation_command_is(command, "3") ||
                   navigation_command_is(command, "neopixel2")) {
            menu = MENU_NEOPIXEL2;
            print_neopixel2_challenge();
        } else if (navigation_command_is(command, "4") ||
                   navigation_command_is(command, "gpio1") ||
                   navigation_command_is(command, "bridge")) {
            gpio1_flag_shown = false;
            menu = MENU_GPIO1;
            print_gpio1_challenge();
        } else if (navigation_command_is(command, "5") ||
                   navigation_command_is(command, "gpio2") ||
                   navigation_command_is(command, "microsolder")) {
            menu = MENU_GPIO2;
            print_gpio2_challenge();
        } else if (navigation_command_is(command, "6") ||
                   navigation_command_is(command, "gpio3") ||
                   navigation_command_is(command, "analyze")) {
            menu = MENU_GPIO3;
            print_gpio3_challenge();
        } else if (navigation_command_is(command, "7") ||
                   navigation_command_is(command, "uart1") ||
                   navigation_command_is(command, "uart")) {
            menu = MENU_UART1;
            print_uart1_challenge();
        } else if (navigation_command_is(command, "8") ||
                   navigation_command_is(command, "uart2") ||
                   navigation_command_is(command, "quiz")) {
            menu = MENU_UART2;
            print_uart2_challenge();
        } else if (navigation_command_is(command, "9") ||
                   navigation_command_is(command, "i2c1")) {
            menu = MENU_I2C1_CHALLENGE;
            print_i2c1_challenge();
        } else if (navigation_command_is(command, "10") ||
                   navigation_command_is(command, "i2c2")) {
            menu = MENU_I2C2_CHALLENGE;
            print_i2c2_challenge();
        } else if (navigation_command_is(command, "11") ||
                   navigation_command_is(command, "gerber")) {
            menu = MENU_GERBER_CHALLENGE;
            print_gerber_challenge();
        } else if (navigation_command_is(command, "12") ||
                   navigation_command_is(command, "schematic")) {
            menu = MENU_SCHEMATIC_CHALLENGE;
            print_schematic_challenge();
        } else {
            submit_challenge_flag(command);
            print_ctf_menu();
        }
        return;
    }

    if (!strcmp(command, "22") || !strcmp(command, "back")) {
        menu = MENU_MAIN;
        print_main_menu();
        return;
    }

    led_animation_t selected;
    if (!strcmp(command, "1") || !strcmp(command, "rainbow")) {
        selected = LED_ANIMATION_RAINBOW;
    } else if (!strcmp(command, "2") || !strcmp(command, "fed_led") ||
               !strcmp(command, "fed-led")) {
        selected = LED_ANIMATION_FED_LED;
    } else if (!strcmp(command, "3") || !strcmp(command, "weird_1") ||
               !strcmp(command, "weird-1") ||
               !strcmp(command, "weird 1")) {
        selected = LED_ANIMATION_WEIRD_1;
    } else if (!strcmp(command, "4") || !strcmp(command, "weird_2") ||
               !strcmp(command, "weird-2") ||
               !strcmp(command, "weird 2")) {
        selected = LED_ANIMATION_WEIRD_2;
    } else if (!strcmp(command, "5") || !strcmp(command, "money_bags") ||
               !strcmp(command, "money bags") ||
               !strcmp(command, "jade-off") ||
               !strcmp(command, "jade/off")) {
        selected = LED_ANIMATION_JADE_OFF;
    } else if (!strcmp(command, "6") ||
               !strcmp(command, "apples_and_oranges") ||
               !strcmp(command, "apples and oranges") ||
               !strcmp(command, "orange-red") ||
               !strcmp(command, "orange/red")) {
        selected = LED_ANIMATION_ORANGE_RED;
    } else if (!strcmp(command, "7") || !strcmp(command, "engage")) {
        selected = LED_ANIMATION_ENGAGE;
    } else if (!strcmp(command, "8") || !strcmp(command, "access")) {
        selected = LED_ANIMATION_ACCESS;
    } else if (!strcmp(command, "9") || !strcmp(command, "agency")) {
        selected = LED_ANIMATION_AGENCY;
    } else if (!strcmp(command, "10") || !strcmp(command, "surf_n_turf") ||
               !strcmp(command, "surf n turf") ||
               !strcmp(command, "surf-n-turf")) {
        selected = LED_ANIMATION_SURF_N_TURF;
    } else if (!strcmp(command, "11") ||
               !strcmp(command, "slow_rainbow_orbit") ||
               !strcmp(command, "slow rainbow orbit")) {
        selected = LED_ANIMATION_SLOW_RAINBOW_ORBIT;
    } else if (!strcmp(command, "12") ||
               !strcmp(command, "dual_comet_chase") ||
               !strcmp(command, "dual comet chase")) {
        selected = LED_ANIMATION_DUAL_COMET_CHASE;
    } else if (!strcmp(command, "13") || !strcmp(command, "breathing_rgb") ||
               !strcmp(command, "breathing rgb") ||
               !strcmp(command, "breathing color cycle")) {
        selected = LED_ANIMATION_BREATHING_COLOR_CYCLE;
    } else if (!strcmp(command, "14") || !strcmp(command, "color_waves") ||
               !strcmp(command, "color waves") ||
               !strcmp(command, "opposing color waves")) {
        selected = LED_ANIMATION_OPPOSING_COLOR_WAVES;
    } else if (!strcmp(command, "15") || !strcmp(command, "energy_pulse") ||
               !strcmp(command, "energy pulse")) {
        selected = LED_ANIMATION_ENERGY_PULSE;
    } else if (!strcmp(command, "16") || !strcmp(command, "molten_fire") ||
               !strcmp(command, "molten fire")) {
        selected = LED_ANIMATION_MOLTEN_FIRE;
    } else if (!strcmp(command, "17") || !strcmp(command, "cyber_alert") ||
               !strcmp(command, "cyber alert") ||
               !strcmp(command, "police/cyber alert")) {
        selected = LED_ANIMATION_POLICE_CYBER_ALERT;
    } else if (!strcmp(command, "18") || !strcmp(command, "aurora_flow") ||
               !strcmp(command, "aurora flow")) {
        selected = LED_ANIMATION_AURORA_FLOW;
    } else if (!strcmp(command, "19") || !strcmp(command, "color_charger") ||
               !strcmp(command, "color charger") ||
               !strcmp(command, "charging meter")) {
        selected = LED_ANIMATION_CHARGING_METER;
    } else if (!strcmp(command, "20") || !strcmp(command, "sparks") ||
               !strcmp(command, "random traveling sparks")) {
        selected = LED_ANIMATION_RANDOM_TRAVELING_SPARKS;
    } else if (!strcmp(command, "21") || !strcmp(command, "off")) {
        selected = LED_ANIMATION_OFF;
    } else {
        puts("Choose an animation from 1-21, or 'back'.");
        print_prompt();
        return;
    }

    if (!led_animation_unlocked(selected)) {
        puts("Animation locked. Complete another CTF challenge to unlock it.");
        print_prompt();
        return;
    }
    badge_leds_select(selected);
    printf("LED animation: %s\n", badge_leds_name());
    menu = MENU_MAIN;
    print_prompt();
}

static void submit_input(void) {
    putchar('\n');
    escape_state = ESCAPE_NONE;

    if (!input_length) {
        if (menu == MENU_MAIN) {
            print_main_menu();
        } else if (menu == MENU_I2C) {
            print_i2c_menu();
        } else if (menu == MENU_I2C_SCRIPT) {
            print_i2c_script_menu();
        } else {
            print_prompt();
        }
        return;
    }

    input_line[input_length] = '\0';
    input_length = 0;
    input_cursor = 0;
    if (menu != MENU_CTF && menu != MENU_WELCOME && menu != MENU_NEOPIXEL1 &&
        menu != MENU_NEOPIXEL2 && menu != MENU_GPIO1 && menu != MENU_GPIO2 &&
        menu != MENU_GPIO3 && menu != MENU_UART1 && menu != MENU_UART2 &&
        menu != MENU_I2C1_CHALLENGE && menu != MENU_I2C2_CHALLENGE &&
        menu != MENU_GERBER_CHALLENGE &&
        menu != MENU_SCHEMATIC_CHALLENGE) {
        lowercase_navigation_command(input_line);
    }
    handle_command(input_line);
}

static void handle_input(int character) {
    if (character == '\n' && skip_lf) {
        skip_lf = false;
        return;
    }
    skip_lf = character == '\r';

    if (escape_state == ESCAPE_STARTED) {
        escape_state = character == '[' || character == 'O'
                           ? ESCAPE_CSI : ESCAPE_NONE;
        return;
    }
    if (escape_state == ESCAPE_CSI) {
        if (character == 'D' && input_cursor) {
            --input_cursor;
        } else if (character == 'C' && input_cursor < input_length) {
            ++input_cursor;
        }
        escape_state = ESCAPE_NONE;
        redraw_input();
        return;
    }
    if (character == 27) {
        escape_state = ESCAPE_STARTED;
        return;
    }
    if (character == '\r' || character == '\n') {
        submit_input();
        return;
    }
    if ((character == '\b' || character == 127) && input_cursor) {
        memmove(input_line + input_cursor - 1, input_line + input_cursor,
                input_length - input_cursor);
        --input_cursor;
        --input_length;
        redraw_input();
        return;
    }
    if (character >= 32 && character <= 126 &&
        input_length < sizeof(input_line) - 1) {
        bool appending = input_cursor == input_length;
        memmove(input_line + input_cursor + 1, input_line + input_cursor,
                input_length - input_cursor);
        input_line[input_cursor++] = (char)character;
        ++input_length;
        if (appending) {
            putchar(character);
            fflush(stdout);
        } else {
            redraw_input();
        }
    }
}

int main(void) {
    badge_hardware_init();
    badge_settings_load(&settings);
    badge_leds_set_brightness(settings.led_brightness_percent);
    badge_ctf_init();
    if (!led_animation_unlocked(
            (led_animation_t)settings.default_led_animation)) {
        settings.default_led_animation = LED_ANIMATION_RAINBOW;
    }
    badge_leds_select((led_animation_t)settings.default_led_animation);
    if (!tune_unlocked(settings.startup_tune)) {
        settings.startup_tune = BADGE_TUNE_STARTUP_JINGLE;
    }
    badge_play_tune(badge_tunes[settings.startup_tune].tones,
                    badge_tunes[settings.startup_tune].tone_count);
    badge_files_usb_init();
    stdio_init_all();
    print_uart_console_menu();

    bool usb_was_connected = false;
    bool button_was_pressed = badge_button_pressed();
    uint32_t last_button_ms = 0;
    uint32_t next_frame_ms = to_ms_since_boot(get_absolute_time());

    while (true) {
        badge_files_task();
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        bool usb_connected = stdio_usb_connected();

        if (usb_connected && !usb_was_connected) {
            if (menu == MENU_UPDI) {
                badge_updiprog_leave();
            }
            menu = MENU_MAIN;
            input_length = 0;
            input_cursor = 0;
            escape_state = ESCAPE_NONE;
            skip_lf = false;
            print_main_menu();
        } else if (!usb_connected) {
            input_length = 0;
            input_cursor = 0;
            escape_state = ESCAPE_NONE;
            skip_lf = false;
        }
        usb_was_connected = usb_connected;

        int character;
        while ((character = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
            handle_input(character);
        }
        if (menu != MENU_UPDI) {
            service_uart1();
            service_uart_quiz_retry(now_ms);
        }
        service_gpio3_output(usb_connected, now_ms);
        reveal_gpio1_flag_if_grounded();

        bool button_is_pressed = badge_button_pressed();
        if (!button_was_pressed && button_is_pressed &&
            now_ms - last_button_ms >= BADGE_BUTTON_DEBOUNCE_MS) {
            last_button_ms = now_ms;
            badge_leds_next(unlocked_led_animation_count());
            if (usb_connected) redraw_input();
        }
        button_was_pressed = button_is_pressed;

        if ((int32_t)(now_ms - next_frame_ms) >= 0) {
            badge_leds_tick();
            next_frame_ms = now_ms + BADGE_FRAME_DELAY_MS;
        }

        tight_loop_contents();
    }
}
