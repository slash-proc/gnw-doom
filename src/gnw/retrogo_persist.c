//
// gnw-doom as a "retro-go app": savestates are RAW SNAPSHOTS of doom's RAM.
//
// The retro-go save/load-state callbacks don't drive doom's own savegame
// machinery at all (that machinery still works, but its storage is an in-RAM
// "SRAM" region — see i_saveg_gnw.c — which simply rides along inside the
// snapshot). A savestate file = header + the raw contents of every RAM
// partition doom owns:
//   1. DTCM .data            (_doom_data_vma_start .. _doom_data_vma_end)
//   2. DTCM scratch .bss     (_dtcm_bss_start      .. _dtcm_bss_end)
//   3. AXISRAM .bss + zone   (_doom_bss_vma_start  .. _zone_end)
// Excluded: the firmware's AHBRAM state, the RAM_UC framebuffers (regenerate),
// and the DTCM stack (live during restore).
//
// The callbacks run on doom's stack DEEP inside the overlay menu, so the work
// is deferred: they just stash the filename and set a flag; doom_persist_pump()
// (called from I_StartFrame, stack-shallow between frames) does the dump or
// restore. Restoring rewrites all globals/zone under our feet, which is safe at
// that depth — afterwards the CLUT is re-pushed and the frame loop continues
// with the restored world (zelda3/smw use the same trick in retro-go).
//
// Snapshots hold pointers into the XIP image, so a savestate is only valid for
// the exact WAD/image that wrote it: identified by the WHD content hash + the
// region table, both validated on load.
//
// NOTE: the firmware's printf (src/libc.c vprintf) supports neither the 'l'
// length modifier nor field width. Stick to plain %x/%u/%d/%s.
//

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "doomdef.h"
#include "doomstat.h"     // players[], consoleplayer
#include "whddata.h"      // whdheader->hash (image/WAD identity)

#include "rg_host.h"      // odroid_system_emu_init + stdio VFS (ABI-backed)
#include "rg_data.h"      // systick_cnt (biased ms clock) + g_doom_time_bias

// Savestate clock continuity (see rg_data.h). Lives in doom .data — i.e.
// INSIDE the snapshot — which is fine: state_load() recomputes it after the
// regions are restored, overwriting whatever value rode in.
uint32_t g_doom_time_bias;

// Linker-provided RAM partition bounds (linker.ld).
extern unsigned long _doom_data_vma_start, _doom_data_vma_end;
extern unsigned long _dtcm_bss_start, _dtcm_bss_end;
extern unsigned long _doom_bss_vma_start, _zone_end;

// CLUT re-push after a restore (i_video_gnw.c owns the table; lcd_set_clut is
// the ABI-routed firmware service).
extern const uint32_t *I_GetClut(void);
extern void lcd_set_clut(const uint32_t *clut, uint16_t count);

#define STATE_MAGIC   0x31535344u   // "DSS1"
#define STATE_VERSION 1u
#define NUM_REGIONS   3u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t wad_hash;              // whdheader->hash: WAD identity
    uint32_t img_crc;               // XIP image fingerprint: exact-build identity
    uint32_t systick_ms;            // snapshot's ms clock (NOT validated; clock rebase)
    uint32_t region_count;
    struct { uint32_t addr, len; } region[NUM_REGIONS];
} state_hdr_t;

// Fingerprint of the running image: a snapshot holds pointers into the XIP
// text/rodata, so it is only valid for the EXACT build (and install address)
// that wrote it — the WAD hash alone misses rebuilds of the same WAD. FNV-1a
// over the first 64K of the image (header, entry, early text), computed once
// at init.
static uint32_t s_img_crc;

static uint32_t img_fingerprint(void)
{
    const uint8_t *p = (const uint8_t *)(0x90000000u + EXTFLASH_OFFSET);
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < 65536u; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static void regions_fill(state_hdr_t *h)
{
    h->magic        = STATE_MAGIC;
    h->version      = STATE_VERSION;
    h->wad_hash     = whdheader->hash;
    h->img_crc      = s_img_crc;
    h->systick_ms   = (uint32_t)systick_cnt;   // biased: epoch of THIS session's globals
    h->region_count = NUM_REGIONS;
    h->region[0].addr = (uint32_t)&_doom_data_vma_start;
    h->region[0].len  = (uint32_t)&_doom_data_vma_end - (uint32_t)&_doom_data_vma_start;
    h->region[1].addr = (uint32_t)&_dtcm_bss_start;
    h->region[1].len  = (uint32_t)&_dtcm_bss_end - (uint32_t)&_dtcm_bss_start;
    h->region[2].addr = (uint32_t)&_doom_bss_vma_start;
    h->region[2].len  = (uint32_t)&_zone_end - (uint32_t)&_doom_bss_vma_start;
}

static void hud_msg(const char *s)
{
    players[consoleplayer].message = s;
}

// --- deferred request, set by the callbacks, executed by the pump -------------
static char s_path[64];
static volatile int s_pending;      // 0 none, 1 save, 2 load

// SWD-readable debug cells in ITCM — OUTSIDE the snapshot regions, so they
// survive a state restore: the last filenames the firmware handed us.
__attribute__((section(".itcram_hot"), aligned(4)))
char g_dbg_save_name[16];
__attribute__((section(".itcram_hot"), aligned(4)))
char g_dbg_load_name[16];

static void dbg_name(char *dst, const char *src)
{
    int i = 0;
    while (src[i] && i < 15) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static bool doom_SaveState(const char *name)
{
    if (!name || strlen(name) >= sizeof s_path) return false;
    dbg_name(g_dbg_save_name, name);
    strcpy(s_path, name);
    s_pending = 1;
    hud_msg("Saving state...");
    return true;
}

static bool doom_LoadState(const char *name)
{
    if (!name || strlen(name) >= sizeof s_path) return false;
    dbg_name(g_dbg_load_name, name);
    strcpy(s_path, name);
    s_pending = 2;
    hud_msg("Loading state...");
    return true;
}

// --- the actual dump/restore (runs at I_StartFrame depth) ---------------------
#define CHUNK 4096u

static void state_save(const char *path)
{
    state_hdr_t h;
    regions_fill(&h);

    void *f = fopen(path, "wb");
    if (!f) { hud_msg("Save state failed (open)."); return; }

    bool ok = fwrite(&h, 1, sizeof h, f) == sizeof h;
    for (uint32_t r = 0; ok && r < h.region_count; r++) {
        const uint8_t *p = (const uint8_t *)h.region[r].addr;
        uint32_t left = h.region[r].len;
        while (ok && left) {
            uint32_t n = left > CHUNK ? CHUNK : left;
            ok = fwrite(p, 1, n, f) == n;
            p += n; left -= n;
        }
    }
    fclose(f);
    printf("gnw-doom state: save %s -> %s\n", path, ok ? "ok" : "FAILED");
    hud_msg(ok ? "State saved." : "Save state failed.");
    if (!ok) remove(path);
}

static void state_load(const char *path)
{
    state_hdr_t want, got;
    regions_fill(&want);

    void *f = fopen(path, "rb");
    if (!f) { hud_msg("No state in this slot."); return; }

    // Validate identity field-by-field — systick_ms is informational (clock
    // rebase), never compared.
    bool ok = fread(&got, 1, sizeof got, f) == sizeof got &&
              got.magic == want.magic && got.version == want.version &&
              got.wad_hash == want.wad_hash && got.img_crc == want.img_crc &&
              got.region_count == want.region_count &&
              memcmp(got.region, want.region, sizeof want.region) == 0;
    if (!ok) {
        fclose(f);
        printf("gnw-doom state: %s rejected (image/WAD mismatch)\n", path);
        hud_msg("State is from another game/build.");
        return;
    }

    // Point of no return: rewrite every doom RAM partition. The stack (top of
    // DTCM, reserved by the linker) and firmware AHBRAM are untouched, so this
    // frame's call chain survives; globals/zone become the saved world.
    for (uint32_t r = 0; ok && r < want.region_count; r++) {
        uint8_t *p = (uint8_t *)want.region[r].addr;
        uint32_t left = want.region[r].len;
        while (ok && left) {
            uint32_t n = left > CHUNK ? CHUNK : left;
            ok = fread(p, 1, n, f) == n;
            p += n; left -= n;
        }
    }
    fclose(f);

    if (!ok) {
        // Half-restored RAM is unrecoverable from software state.
        printf("gnw-doom state: %s short read — resetting\n", path);
        *(volatile uint32_t *)0xE000ED0C = (0x5FAu << 16) | (1u << 2); // SYSRESETREQ
        while (1) {}
    }

    // Clock continuity: the restored globals reference the saving session's ms
    // epoch. Rebase the biased clock so systick_cnt continues from the
    // snapshot's timestamp (overwrites the bias value the restore brought in).
    g_doom_time_bias = got.systick_ms - gnw_abi()->HAL_GetTick();

    lcd_set_clut(I_GetClut(), 256);   // restored palette -> panel
    printf("gnw-doom state: load %s ok\n", path);
    hud_msg("State loaded.");
}

// Called once per frame from I_StartFrame (i_video_gnw.c): stack-shallow,
// between-frames quiescent point.
void doom_persist_pump(void)
{
    int p = s_pending;
    if (!p) return;
    s_pending = 0;
    if (p == 1) state_save(s_path);
    else        state_load(s_path);
}

// --- SRAM channel: lightweight settings (volume), as before -------------------
#define SETTINGS_MAGIC 0x47544553UL  // "SETG"
#define SETTINGS_PATH "/data/doom.sram"

static void doom_SramSave(void)
{
    uint32_t blob[4] = { SETTINGS_MAGIC, (uint32_t)sfxVolume, (uint32_t)musicVolume, 0 };
    void *f = fopen(SETTINGS_PATH, "wb");
    int r = f ? (int)fwrite(blob, 1, sizeof blob, f) : -1;
    if (f) fclose(f);
    printf("gnw-doom persist: SramSave sfx=%d mus=%d -> %d\n", sfxVolume, musicVolume, r);
}

void doom_persist_init(void)
{
    s_img_crc = img_fingerprint();

    odroid_system_emu_init(doom_LoadState, doom_SaveState,
                           NULL, NULL, NULL, doom_SramSave);

    // Restore settings before S_Init(sfxVolume*8, musicVolume*8) runs in
    // D_DoomMain, so the saved volume takes effect this boot.
    uint32_t blob[4] = { 0 };
    void *rf = fopen(SETTINGS_PATH, "rb");
    int r = rf ? (int)fread(blob, 1, sizeof blob, rf) : -1;
    if (rf) fclose(rf);
    if (r >= (int)sizeof blob && blob[0] == SETTINGS_MAGIC) {
        if (blob[1] <= 15) sfxVolume   = (int)blob[1];
        if (blob[2] <= 15) musicVolume = (int)blob[2];
        printf("gnw-doom persist: settings restored sfx=%d mus=%d\n", sfxVolume, musicVolume);
    }
    printf("gnw-doom persist: ready (savestate = RAM snapshot)\n");
}
