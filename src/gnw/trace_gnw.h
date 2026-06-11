//
// Pipeline tracer (build with TRACE=1): every render/audio stage logs
// begin/end events with the DWT cycle counter (280 MHz, 3.57 ns) into a
// 512 KB AXISRAM ring buffer; scripts/debug/tracepull.py drains it over SWD
// and writes the per-frame timing report. Compiled out entirely otherwise.
//
#ifndef DOOMX_TRACE_GNW_H
#define DOOMX_TRACE_GNW_H

#include <stdint.h>

// Event IDs. *_BEG/*_END pairs nest; MARKs are points with an argument.
enum {
    TEV_NONE = 0,
    TEV_FRAME_MARK,      // arg = frame number low 16 (present boundary)
    TEV_TICS_BEG, TEV_TICS_END,             // TryRunTics (game logic + wait)
    TEV_GTIC_BEG, TEV_GTIC_END,             // one G_Ticker (pure game logic)
    TEV_RENDER_BEG, TEV_RENDER_END,         // D_Display whole render
    TEV_BSP_BEG, TEV_BSP_END,               // R_RenderPlayerView BSP+segs walk
    TEV_FLATS_BEG, TEV_FLATS_END,           // draw_visplanes total
    TEV_FLATDEC_BEG, TEV_FLATDEC_END,       // one flat decode, arg=picnum
    TEV_PATCHDEC_BEG, TEV_PATCHDEC_END,     // one patch decoder fetch, arg=patch
    TEV_REGCOLS_BEG, TEV_REGCOLS_END,       // draw_regular_columns (walls/sprites)
    TEV_FUZZ_BEG, TEV_FUZZ_END,             // fuzz columns
    TEV_OVERLAY_BEG, TEV_OVERLAY_END,       // ST/HU/menu drawers (list build)
    TEV_COMPOSE_BEG, TEV_COMPOSE_END,       // i_video compose to LTDC fb
    TEV_MIX_BEG, TEV_MIX_END,               // SAI mixer batch, arg=samples
    TEV_OPL_BEG, TEV_OPL_END,               // emu8950 chunk render, arg=samples
    TEV_IDLE_BEG, TEV_IDLE_END,             // pacer/tic waits (true idle)
    TEV_WIPE_MARK,                          // wipe frame presented
    TEV_CMP_BASE,                           // compose: base scanlines, arg=us
    TEV_CMP_OVERLAY,                        // compose: vpatch overlays, arg=us
    TEV_CMP_OUT,                            // compose: LTDC row writes, arg=us
    TEV_LOAD_BEG, TEV_LOAD_END,             // P_SetupLevel (the wipe-hitch span)
    TEV_COUNT
};

#if DOOMX_TRACE

typedef struct {
    uint32_t cyc;       // DWT->CYCCNT at event
    uint16_t ev;
    uint16_t arg;
} trace_entry_t;

// 256 KB / 8 = 32768 entries (~10-18 s of full-detail tracing); sized so
// the patch pixel cache and the ring coexist in AXISRAM.
#define TRACE_ENTRIES 32768u
extern trace_entry_t trace_buf[TRACE_ENTRIES];
extern volatile uint32_t trace_head;   // monotonically increasing; index = head % TRACE_ENTRIES

void trace_init(void);

static inline void trace_emit(uint16_t ev, uint16_t arg)
{
    extern volatile uint32_t trace_head;
    uint32_t h = trace_head;
    trace_entry_t *e = &trace_buf[h & (TRACE_ENTRIES - 1u)];
    e->cyc = *(volatile uint32_t *)0xE0001004; // DWT->CYCCNT
    e->ev = ev;
    e->arg = arg;
    trace_head = h + 1;
}

#define TRACE_EVT(ev, arg) trace_emit((ev), (uint16_t)(arg))
#else
#define TRACE_EVT(ev, arg) ((void)0)
static inline void trace_init(void) {}
#endif

// Level-load audio pump (functional, not tracing): P_SetupLevel can run long
// enough to drain the ~85 ms SAI buffer, so the DMA loops a stale tone (the
// "carried tone" hitch). Pump the mixer between load stages; each stage is well
// under the buffer. No-op on hosts/upstream (see tracehooks.h).
#ifndef __cplusplus
void I_UpdateSound(void);   // C TUs only; pd_render.cpp has its own extern "C" decl
void pd_warm_flat_cache(void);
void pd_warm_sprite_cache(void);
#endif
#define LOAD_PUMP() I_UpdateSound()

// Warm the flat + sprite decode caches during the level-load pause instead of
// storming decodes over the first rendered frames (both pump the mixer too).
#define LOAD_WARM() do { pd_warm_flat_cache(); pd_warm_sprite_cache(); } while (0)

#endif
