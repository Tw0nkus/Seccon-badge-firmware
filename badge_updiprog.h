#ifndef BADGE_UPDIPROG_H
#define BADGE_UPDIPROG_H

#include <stdbool.h>

void badge_updiprog_enter(void);
void badge_updiprog_leave(void);
const char *badge_updiprog_prompt(void);
bool badge_updiprog_handle_command(char *command);

#endif
