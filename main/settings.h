#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cbm.h"

/* Machine settings kept in NVS (namespace "cbm") */
typedef struct {
    uint8_t model;          /* cbm_model_t */
    uint8_t joystick;       /* port the keypad joystick is in: 1 or 2 */
} settings_t;

extern settings_t settings;

void settings_load(void);
void settings_save(void);
