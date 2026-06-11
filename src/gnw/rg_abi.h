//
// rg_abi.h — bind to the firmware's versioned ABI table (the decoupling seam).
//
// Instead of resolving firmware functions by address (--just-symbols, which ties
// this binary to ONE firmware build), the app reads g_firmware_abi at VTOR+0x400 and
// calls THROUGH it. Same mechanism as retro-go's gw_firmware_abi. One gnw-doom.bin
// then runs on any firmware that publishes a compatible (version+size) table,
// surviving firmware rebuilds. See ../../include/gnw_firmware_abi.h (-I../include).
//
#ifndef RG_ABI_H
#define RG_ABI_H

#include "gnw_firmware_abi.h"

// The firmware ABI table, located via the active vector table base (VTOR) so it
// resolves correctly whichever flash bank the firmware runs from.
static inline const gnw_firmware_abi_t *gnw_abi(void)
{
    uint32_t vtor = *(volatile uint32_t *)GNW_VTOR_ADDRESS;
    return (const gnw_firmware_abi_t *)(vtor + GNW_FIRMWARE_ABI_OFFSET);
}

// Validate version + size before trusting the table (call once at startup; a
// mismatch means the app and firmware ABI are incompatible).
static inline int gnw_abi_ok(void)
{
    const gnw_firmware_abi_t *a = gnw_abi();
    return a->version == GNW_FIRMWARE_ABI_VERSION &&
           a->size >= sizeof(gnw_firmware_abi_t);
}

#endif // RG_ABI_H
