//
// abi_stubs.c — resolve the firmware surface through the ABI table, not symbols.
//
// Defines every firmware function gnw-doom references as a thin wrapper that calls
// through g_firmware_abi (rg_abi.h). With these present, the link no longer needs
// --just-symbols=firmware.out — one gnw-doom.bin runs on any firmware publishing a
// compatible ABI. memcpy/memset stay local (fastmem.c, hot path); the firmware's
// sscanf is a no-op stub upstream, so we keep a local no-op (callers fall back to
// defaults exactly as before).
//
#include "rg_abi.h"
#include <stddef.h>
#include <stdarg.h>

#define A (gnw_abi())

// --- string / memory ---------------------------------------------------------
int    memcmp(const void *a, const void *b, size_t n)        { return A->memcmp(a, b, n); }
void  *memmove(void *d, const void *s, size_t n)             { return A->memmove(d, s, n); }
size_t strlen(const char *s)                                 { return A->strlen(s); }
char  *strncpy(char *d, const char *s, size_t n)             { return A->strncpy(d, s, n); }
int    strcmp(const char *a, const char *b)                  { return A->strcmp(a, b); }
int    strncmp(const char *a, const char *b, size_t n)       { return A->strncmp(a, b, n); }
char  *strstr(const char *h, const char *n)                  { return A->strstr(h, n); }

// --- ctype --------------------------------------------------------------------
int    tolower(int c)                                        { return A->tolower(c); }
int    toupper(int c)                                        { return A->toupper(c); }

// abs, malloc/free/realloc, printf/snprintf/vprintf/vsnprintf, sscanf, strcpy and
// strdup are NOT in retro-go's ABI -> gnw-doom owns them locally (gnw_libc.c).

// --- firmware platform services ----------------------------------------------
void     audio_start_playing(uint16_t len)       { A->audio_start_playing(len); }
int16_t *audio_get_active_buffer(void)           { return A->audio_get_active_buffer(); }
void     audio_clear_active_buffer(void)         { A->audio_clear_active_buffer(); }
void     audio_clear_inactive_buffer(void)       { A->audio_clear_inactive_buffer(); }

void common_ingame_overlay(void)                 { A->common_ingame_overlay(); }
/* retro-go's standard in-game UX: PAUSE menu + macros (save/load, volume,
 * brightness, screenshot) and the bare-POWER save+sleep handler all live in
 * this loop — feed it the real joystick every input poll (i_input_gnw.c). */
void common_emu_input_loop(void *js, void *game_options, void *repaint)
{
    A->common_emu_input_loop(js, game_options, repaint);
}
void odroid_input_read_gamepad(void *out)        { A->wdog_refresh(); A->odroid_input_read_gamepad(out); }
void odroid_system_switch_app(int app)           { A->odroid_system_switch_app(app); }

// stdio VFS (retro-go's plugin file API; FILE* is opaque void* across the ABI).
void  *fopen(const char *path, const char *mode)                    { return A->fopen(path, mode); }
int    fclose(void *stream)                                         { return A->fclose(stream); }
size_t fread(void *ptr, size_t size, size_t n, void *stream)        { return A->fread(ptr, size, n, stream); }
size_t fwrite(const void *ptr, size_t size, size_t n, void *stream) { return A->fwrite(ptr, size, n, stream); }
int    remove(const char *path)                                     { return A->remove(path); }

void  lcd_set_clut(const uint32_t *clut, uint16_t count) { A->lcd_set_clut(clut, count); }
void  lcd_setup_framebuffers(int lcd_mode)   { A->lcd_setup_framebuffers(lcd_mode); }
void *lcd_get_active_buffer(void)            { return A->lcd_get_active_buffer(); }
void *lcd_get_inactive_buffer(void)          { return A->lcd_get_inactive_buffer(); }
/* Watchdog: real retro-go runs a WWDG that emulators refresh every frame;
 * piggyback on the two calls doom makes constantly (frame swap + input poll)
 * so the dog stays fed through gameplay, menus, and demo loops. */
void  lcd_swap(void)                         { A->wdog_refresh(); A->lcd_swap(); }

void odroid_system_emu_init(void *l, void *s, void *ss, void *sd, void *w, void *sr)
{
    A->odroid_system_emu_init(l, s, ss, sd, w, sr);
}
