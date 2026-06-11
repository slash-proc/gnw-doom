//
// rg_host.h — retro-go host API declarations the persistence layer uses.
//
// Everything here resolves at runtime through the firmware ABI table
// (rg_abi.h / abi_stubs.c) — no firmware headers, no link-time coupling.
// These ARE retro-go symbols (odroid_system.h in real retro-go).
//

#ifndef RG_HOST_H
#define RG_HOST_H

#include <stdint.h>

#include <stdbool.h>

// retro-go's odroid_system persistence API: the app registers its save/load
// callbacks; the host owns path construction and drives them by slot.
typedef bool  (*rg_state_handler_t)(const char *filename);
typedef void *(*rg_screenshot_handler_t)(void);
typedef void  (*rg_shutdown_handler_t)(void);
typedef void  (*rg_wakeup_handler_t)(void);
typedef void  (*rg_sram_handler_t)(void);
void odroid_system_emu_init(rg_state_handler_t load, rg_state_handler_t save,
                            rg_screenshot_handler_t screenshot, rg_shutdown_handler_t shutdown,
                            rg_wakeup_handler_t wakeup, rg_sram_handler_t sram_save);

// --- Storage: stdio VFS (retro-go's plugin file API; FILE* is opaque) ---------
// The host backs these with littlefs; the app names files, never the
// filesystem. Mirrors retro-go's fopen/fread/...
#include <stddef.h>
extern void  *fopen(const char *path, const char *mode);
extern int    fclose(void *stream);
extern size_t fread(void *ptr, size_t size, size_t nmemb, void *stream);
extern size_t fwrite(const void *ptr, size_t size, size_t nmemb, void *stream);
extern int    remove(const char *path);

#endif // RG_HOST_H
