//
// GNW input: replaces src/pico/i_input.c with the hardware-validated button
// logic from doomgeneric/doomgeneric_stm32.c, emitting engine events via
// D_PostEvent.
//
// Button layout (bit -> Doom key; bindings overridden in I_InputInit):
//   A          fire; selects/confirms in menus
//   B          strafe modifier + use (key_strafe and key_use share KEY_RALT)
//   SELECT     run (key_speed / KEY_RSHIFT) — unmapped from strafe by request
//   START      open/close menu (KEY_ENTER)
//   GAME       same as START (deferred tap: half of the overlay combo)
//   TIME       automap toggle (deferred tap, the combo's other half)
//   PAUSE+POWER standby
//

#include <stdio.h>

#include "doomtype.h"
#include "doomkeys.h"
#include "d_event.h"
#include "i_input.h"
#include "rg_data.h"    // systick_cnt (HAL_GetTick) for tap-vs-hold timing
#include "rg_input.h"   // odroid_gamepad_state_t + odroid_input_read_gamepad

// Remote-input mailbox: a DTCM cell the SWD debug host writes button state into.
#define SRAM_REMOTE_INPUT_ADDR  0x2001FFF4UL

#define BTN_BIT_TIME    7
#define BTN_BIT_SELECT  8
#define BTN_BIT_GAME    9
#define BTN_BIT_PAUSE   10
// POWER is firmware-owned (not exposed in the gamepad state) -> no BTN_BIT_POWER.

#define GNW_KEY_FIRE    KEY_RCTRL   // default key_fire binding

float mouse_acceleration = 1.0f;
int mouse_threshold = 10;
int novert = 0;

extern void power_off(void);
extern void D_PostEvent(event_t *ev);

static uint32_t last_buttons;

static void post_key(int pressed, int key)
{
    event_t event;
    event.type = pressed ? ev_keydown : ev_keyup;
    event.data1 = key;
    event.data2 = 0;
    event.data3 = 0;
    D_PostEvent(&event);
}

// Control scheme below; the doom key_* bindings are set in I_InputInit. Tap-vs-hold
// is timed (HOLD_MS); a combo or a fired hold suppresses the tap. GAME tap ->
// retro-go overlay; TIME tap -> menu, hold -> automap; GAME+TIME (held) -> perf HUD
// (read in perf_gnw.c). PAUSE tap -> next weapon, hold+UP/DOWN -> prev/next weapon.
// START/A = fire/select; SELECT = run; B hold = strafe, B tap = use/back.
#define HOLD_MS 200
extern void common_ingame_overlay(void);

// Bit index in the held-key word -> doom scancode.
static int held_key(int i)
{
    switch (i) {
        case 0: return KEY_UPARROW;
        case 1: return KEY_DOWNARROW;
        case 2: return KEY_LEFTARROW;
        case 3: return KEY_RIGHTARROW;
        case 4: return GNW_KEY_FIRE;   // A (fire)
        case 5: return KEY_RALT;       // START (strafe, in-game only)
        case 6: return KEY_ENTER;      // A (menu select)
    }
    return 0;
}

void I_GetEvent(void)
{
    // Firmware-owned input (retro-go model): read the gamepad state and rebuild
    // the button mask this layer already works in. POWER is not exposed — the
    // firmware owns the power button / standby (input.c). Save/load is likewise
    // firmware-triggered via the handlers we register (odroid_system_emu_init).
    odroid_gamepad_state_t js;
    odroid_input_read_gamepad(&js);
    uint32_t current_buttons =
        ((uint32_t)js.values[ODROID_INPUT_UP]     << 0) |
        ((uint32_t)js.values[ODROID_INPUT_DOWN]   << 1) |
        ((uint32_t)js.values[ODROID_INPUT_LEFT]   << 2) |
        ((uint32_t)js.values[ODROID_INPUT_RIGHT]  << 3) |
        ((uint32_t)js.values[ODROID_INPUT_A]      << 4) |
        ((uint32_t)js.values[ODROID_INPUT_B]      << 5) |
        ((uint32_t)js.values[ODROID_INPUT_START]  << 6) |
        ((uint32_t)js.values[ODROID_INPUT_Y]      << 7) |   // TIME
        ((uint32_t)js.values[ODROID_INPUT_SELECT] << 8) |
        ((uint32_t)js.values[ODROID_INPUT_X]      << 9) |   // GAME
        ((uint32_t)js.values[ODROID_INPUT_MENU]   << 10);   // PAUSE

    uint32_t now  = (uint32_t)systick_cnt;
    extern boolean menuactive;
    int up    = (current_buttons >> 0) & 1, down = (current_buttons >> 1) & 1;
    int game  = (current_buttons >> BTN_BIT_GAME) & 1;
    int timeb = (current_buttons >> BTN_BIT_TIME) & 1;
    int pause = (current_buttons >> BTN_BIT_PAUSE) & 1;
    int sel   = (current_buttons >> BTN_BIT_SELECT) & 1;
    int bbtn  = (current_buttons >> 5) & 1;
    int a     = (current_buttons >> 4) & 1;
    int start = (current_buttons >> 6) & 1;

    // GAME: tap -> firmware overlay; suppressed if it formed the GAME+TIME perf
    // combo. The overlay blocks, so release held keys + reset edge state first.
    static int game_down, game_combo;
    if (game) {
        if (!game_down) { game_down = 1; game_combo = 0; }
        if (timeb) game_combo = 1;
    } else if (game_down) {
        game_down = 0;
        if (!game_combo) {
            for (int i = 0; i < 7; i++)
                if (last_buttons & (1u << i)) post_key(0, held_key(i));
            last_buttons = 0;
            common_ingame_overlay();
            return;
        }
    }

    // TIME: tap -> automap (TAB); suppressed if it formed the GAME+TIME perf combo.
    static int time_down, time_combo;
    if (timeb) {
        if (!time_down) { time_down = 1; time_combo = 0; }
        if (game) time_combo = 1;
    } else if (time_down) {
        time_down = 0;
        if (!time_combo) { post_key(1, KEY_TAB); post_key(0, KEY_TAB); }   // automap
    }

    // PAUSE: in-game "Start" -> open/close the menu (ESC toggles).
    static int pause_down;
    if (pause) {
        if (!pause_down) { pause_down = 1; post_key(1, KEY_ESCAPE); post_key(0, KEY_ESCAPE); }
    } else pause_down = 0;

    // SELECT: tap -> next weapon; hold + UP/DOWN -> prev/next (UP/DOWN stop moving
    // then). key_prevweapon='[', key_nextweapon=']' (bound in I_InputInit).
    static int sel_down, sel_used, up_p, down_p;
    if (sel) {
        if (!sel_down) { sel_down = 1; sel_used = 0; up_p = up; down_p = down; }
        if (up   && !up_p)   { post_key(1, '['); post_key(0, '['); sel_used = 1; }
        if (down && !down_p) { post_key(1, ']'); post_key(0, ']'); sel_used = 1; }
        up_p = up; down_p = down;
    } else if (sel_down) {
        sel_down = 0;
        if (!sel_used) { post_key(1, ']'); post_key(0, ']'); }   // tap = next weapon
    }

    // B: hold -> run (KEY_RSHIFT); tap -> use/back. BT_USE is latched per tic from
    // gamekeydown, so a same-poll down+up can fall between tics and be missed — hold
    // ' ' for a short pulse (>1 tic). Menu "back" is edge-triggered, so it still sees it.
    static int b_down, b_run; static uint32_t b_t0;
    static int use_on; static uint32_t use_t0;
    if (bbtn) {
        if (!b_down) { b_down = 1; b_run = 0; b_t0 = now; }
        else if (!b_run && (uint32_t)(now - b_t0) > HOLD_MS) { b_run = 1; post_key(1, KEY_RSHIFT); }
    } else if (b_down) {
        b_down = 0;
        if (b_run) post_key(0, KEY_RSHIFT);
        else { post_key(1, ' '); use_on = 1; use_t0 = now; }   // begin use pulse
    }
    if (use_on && (uint32_t)(now - use_t0) > 40) { post_key(0, ' '); use_on = 0; }

    // Double-tap UP or B -> jump (key_jump = '/'). B is triple-duty (run/use/jump).
    // Jump latches per tic like use, so hold '/' for a short pulse. UP's double-tap
    // is ignored while SELECT is held (UP cycles weapons there).
    static int up_prev_raw, b_prev_raw; static uint32_t up_tap_t, b_tap_t;
    static int jump_on; static uint32_t jump_t0;
    int jump_now = 0;
    if (up && !up_prev_raw) {
        if (!sel && (uint32_t)(now - up_tap_t) < 250) jump_now = 1;
        up_tap_t = now;
    }
    up_prev_raw = up;
    if (bbtn && !b_prev_raw) {
        if ((uint32_t)(now - b_tap_t) < 250) jump_now = 1;
        b_tap_t = now;
    }
    b_prev_raw = bbtn;
    if (jump_now && !jump_on) { post_key(1, '/'); jump_on = 1; jump_t0 = now; }
    if (jump_on && (uint32_t)(now - jump_t0) > 40) { post_key(0, '/'); jump_on = 0; }

    // START: strafe in-game (held key below); in a menu it acts like Start (close).
    static int start_menu_prev;
    if (menuactive && start && !start_menu_prev) { post_key(1, KEY_ESCAPE); post_key(0, KEY_ESCAPE); }
    start_menu_prev = start;

    // Held keys (edge-detected): fire (A), strafe (START, in-game only), arrows, and
    // menu-select (A -> ENTER). UP/DOWN are gated out while SELECT is held (weapon
    // cycling); START's strafe drops in a menu so it doesn't nudge the cursor.
    uint32_t cur =
        ((uint32_t)(up   && !sel)            << 0) |
        ((uint32_t)(down && !sel)            << 1) |
        ((uint32_t)((current_buttons >> 2) & 1) << 2) |   // LEFT
        ((uint32_t)((current_buttons >> 3) & 1) << 3) |   // RIGHT
        ((uint32_t)a                         << 4) |      // A -> fire
        ((uint32_t)(start && !menuactive)    << 5) |      // START -> strafe (in-game)
        ((uint32_t)a                         << 6);       // A -> ENTER (menu select)
    uint32_t ch = cur ^ last_buttons;
    for (int i = 0; i < 7; i++)
        if (ch & (1u << i)) post_key((cur >> i) & 1, held_key(i));
    last_buttons = cur;
}

void I_GetEventTimeout(int timeout_ms)
{
    extern void I_Sleep(int ms);
    I_GetEvent();
    if (timeout_ms > 0)
        I_Sleep(timeout_ms);
}

void I_InputInit(void)
{
    // Button GPIOs are configured by the firmware (it owns the hardware and reads
    // them via gnw_input_read); a retro-go core must not touch raw GPIO.

    // The remote-input mailbox lives in DTCM, which Standby powers off — after
    // a wake it holds garbage that reads as permanently-held buttons. Clear it.
    *(volatile uint32_t *)SRAM_REMOTE_INPUT_ADDR = 0;

    // Handheld control bindings (override m_controls.c defaults; there is no
    // config file to clobber them).
    {
        extern key_type_t key_use, key_strafe, key_speed, key_fire,
                          key_prevweapon, key_nextweapon,
                          key_menu_forward, key_menu_confirm,
                          key_menu_activate, key_menu_back, key_menu_abort;
        key_fire          = GNW_KEY_FIRE;   // A
        key_use           = ' ';            // B tap: use / open
        key_strafe        = KEY_RALT;       // START hold: strafe
        key_speed         = KEY_RSHIFT;     // B hold: run
        key_prevweapon    = '[';            // SELECT + UP
        key_nextweapon    = ']';            // SELECT tap / SELECT + DOWN
        key_menu_forward  = KEY_ENTER;      // A selects menu items
        key_menu_confirm  = KEY_ENTER;      // A answers Y/N prompts
        key_menu_activate = KEY_ESCAPE;     // PAUSE opens / closes; START closes
        key_menu_back     = ' ';            // B (use) backs out of a menu level
        key_menu_abort    = ' ';            // B cancels Y/N prompts
    }
}

// No keyboard: text input (savegame names) falls back to default slot names.
void I_StartTextInput(int x1, int y1, int x2, int y2)
{
}

void I_StopTextInput(void)
{
}

int GetTypedChar(int scancode, boolean shiftdown)
{
    return 0;
}

void I_ReadMouse(void)
{
}

void I_BindInputVariables(void)
{
}
