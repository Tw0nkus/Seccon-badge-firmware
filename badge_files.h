#ifndef BADGE_FILES_H
#define BADGE_FILES_H

#include <stdbool.h>
#include <stddef.h>

enum {
    BADGE_FILES_CAPACITY_BYTES = 3 * 1024 * 1024,
};

typedef enum {
    BADGE_FILES_READY,
    BADGE_FILES_CREATED,
    BADGE_FILES_REPAIRED,
    BADGE_FILES_UNSUPPORTED,
    BADGE_FILES_ERROR,
} badge_files_status_t;

void badge_files_usb_init(void);
void badge_files_task(void);
badge_files_status_t badge_files_prepare(void);
bool badge_files_print_listing(void);
bool badge_files_read_file(const char *name, char *buffer, size_t capacity,
                          size_t *length);
bool badge_files_enter_usb_mode(void);

#endif
