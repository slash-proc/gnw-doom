//
// Pipeline tracer (build with TRACE=1): every render/audio stage logs
// begin/end events with the DWT cycle counter (280 MHz, 3.57 ns). Compiled
// out entirely otherwise.
//
// PRIORITY RETENTION. A flat ring would fill with whatever ran most recently —
// mostly fast, uninteresting frames. Instead events accumulate per-frame into
// a small pool of slots; at each frame boundary the just-finished frame is
// KEPT only if it is slow enough to matter (slower than the fastest frame the
// pool currently holds, once the pool is full). So the pool converges to the N
// WORST frames seen across the whole session — exactly the ones worth reading —
// in a fraction of the RAM a ring would need. scripts/debug/tracepull.py drains
// it over SWD and writes the per-frame timing report.
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

// Pool geometry (override with -D). NUM_SLOTS frames retained, each up to
// SLOT_EVENTS events. Total = NUM_SLOTS * (16 + SLOT_EVENTS*8) bytes.
//   default 8 * (16 + 2048*8) = ~128 KB — the 8 worst frames at full detail.
// A frame emitting more than SLOT_EVENTS events is truncated (flagged).
#ifndef TRACE_NUM_SLOTS
#define TRACE_NUM_SLOTS  8u
#endif
#ifndef TRACE_SLOT_EVENTS
#define TRACE_SLOT_EVENTS 2048u
#endif

typedef struct {
    uint32_t frame_no;   // sequential id of the frame this slot holds
    uint32_t dur_cyc;    // frame wall time (FRAME_MARK..next FRAME_MARK)
    uint32_t count;      // events stored (<= TRACE_SLOT_EVENTS)
    uint32_t truncated;  // events dropped past SLOT_EVENTS
    trace_entry_t ev[TRACE_SLOT_EVENTS];
} trace_slot_t;

// The slot pool + the index of the slot the in-progress frame writes into.
extern trace_slot_t trace_slots[TRACE_NUM_SLOTS];
extern volatile uint32_t trace_stage;   // staging slot index

void trace_init(void);
// Called at each present boundary (FRAME_MARK): score the finished frame and
// either keep it (evicting the fastest kept frame when full) or discard it.
void trace_frame_boundary(uint32_t cyc);

static inline void trace_emit(uint16_t ev, uint16_t arg)
{
    uint32_t c = *(volatile uint32_t *)0xE0001004;   // DWT->CYCCNT (read first)
    if (ev == TEV_FRAME_MARK)
        trace_frame_boundary(c);                     // may re-stage
    trace_slot_t *s = &trace_slots[trace_stage];
    uint32_t n = s->count;
    if (n < TRACE_SLOT_EVENTS) {
        s->ev[n].cyc = c;
        s->ev[n].ev  = ev;
        s->ev[n].arg = arg;
        s->count = n + 1;
    } else {
        s->truncated++;
    }
}

#define TRACE_EVT(ev, arg) trace_emit((ev), (uint16_t)(arg))
#else
#define TRACE_EVT(ev, arg) ((void)0)
static inline void trace_init(void) {}
#endif

// Level-load audio pump (functional, not tracing): P_SetupLevel can run long
// enough to drain the ~85 ms SAI buffer, so the DMA loops a stale tone (the
// "carried tone" hitch). Pump the mixer between load stages; each stage is well
// under the buffer. No-op on hosts/upstream (see tracehooks.h). Defined for ALL
// DOOMX builds, not just TRACE — these are not instrumentation.
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
