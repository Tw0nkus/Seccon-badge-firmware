#ifndef BADGE_TUNES_H
#define BADGE_TUNES_H

#include <stddef.h>

#include "badge_hardware.h"

typedef enum {
    BADGE_TUNE_G_THANG,
    BADGE_TUNE_MARIO,
    BADGE_TUNE_SIMPSONS,
    BADGE_TUNE_SCALE,
    BADGE_TUNE_ARPEGGIO,
    BADGE_TUNE_SIREN,
    BADGE_TUNE_STARTUP_JINGLE,
    BADGE_TUNE_COUNT,
} badge_tune_id_t;

typedef struct {
    const char *name;
    const char *command;
    const badge_tone_t *tones;
    size_t tone_count;
} badge_tune_definition_t;

extern const badge_tune_definition_t badge_tunes[BADGE_TUNE_COUNT];

int badge_tune_index_from_command(const char *command);

#endif
