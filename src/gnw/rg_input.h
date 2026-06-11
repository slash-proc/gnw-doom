//
// rg_input.h — retro-go's odroid_input types, vendored so the core reads input
// the retro-go way (odroid_input_read_gamepad into an odroid_gamepad_state_t)
// without including the firmware tree. These ARE retro-go symbols.
//
#ifndef RG_INPUT_H
#define RG_INPUT_H

#include <stdint.h>

typedef enum {
    ODROID_INPUT_UP = 0,
    ODROID_INPUT_RIGHT,
    ODROID_INPUT_DOWN,
    ODROID_INPUT_LEFT,
    ODROID_INPUT_SELECT,
    ODROID_INPUT_START,
    ODROID_INPUT_A,
    ODROID_INPUT_B,
    ODROID_INPUT_MENU,     // PAUSE/SET (overlay trigger)
    ODROID_INPUT_VOLUME,   // PAUSE alias
    ODROID_INPUT_X,        // GAME
    ODROID_INPUT_Y,        // TIME
    ODROID_INPUT_MAX
    // NOTE: no POWER — the firmware owns the power button / standby.
} odroid_input_t;

typedef struct {
    uint8_t values[ODROID_INPUT_MAX];
} odroid_gamepad_state_t;

void odroid_input_read_gamepad(odroid_gamepad_state_t *out_state);

#endif // RG_INPUT_H
