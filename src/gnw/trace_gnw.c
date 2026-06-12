//
// Pipeline tracer buffer + frame-priority retention. See trace_gnw.h.
//
#include "trace_gnw.h"

#if DOOMX_TRACE

// The slot pool lives in .trace_buf (linker carves it from AXISRAM; TRACE
// builds relax ZONE_MIN in linker.ld to make room — gameplay zone use is far
// below the release floor). There is no separate scratch frame: the
// in-progress frame writes straight into trace_stage, and the boundary
// decides whether to commit (advance to a fresh staging slot) or discard
// (reset the same slot and reuse it for the next frame).
__attribute__((section(".trace_buf"))) trace_slot_t trace_slots[TRACE_NUM_SLOTS];
volatile uint32_t trace_stage;          // slot the current frame fills

static uint32_t s_frame_no;             // sequential frame counter
static uint32_t s_stage_start;          // CYCCNT at the staging frame's open
static uint32_t s_filled;               // committed slots so far (<= NUM_SLOTS)

// Pick the next staging slot: a still-empty one while filling, else the
// fastest KEPT slot (smallest dur_cyc) — the one we are willing to overwrite
// next, and only when the incoming frame proves slower.
static uint32_t pick_victim(void)
{
    if (s_filled < TRACE_NUM_SLOTS)
        return s_filled;                // next empty slot
    uint32_t best = 0;
    for (uint32_t i = 1; i < TRACE_NUM_SLOTS; i++)
        if (trace_slots[i].dur_cyc < trace_slots[best].dur_cyc)
            best = i;
    return best;
}

void trace_frame_boundary(uint32_t cyc)
{
    trace_slot_t *cur = &trace_slots[trace_stage];

    // First boundary just opens the first frame.
    if (s_stage_start == 0 && cur->count == 0) {
        cur->frame_no = s_frame_no;
        s_stage_start = cyc ? cyc : 1u;
        return;
    }

    // Score the frame that just ended.
    cur->dur_cyc = cyc - s_stage_start;

    int keep;
    if (s_filled < TRACE_NUM_SLOTS) {
        keep = 1;                        // still filling: keep everything
    } else {
        // Full: keep only if slower than the fastest frame we currently hold
        // (i.e. it belongs among the worst). Equal-or-faster frames drop.
        uint32_t fastest = trace_slots[0].dur_cyc;
        for (uint32_t i = 1; i < TRACE_NUM_SLOTS; i++)
            if (trace_slots[i].dur_cyc < fastest) fastest = trace_slots[i].dur_cyc;
        keep = cur->dur_cyc > fastest;
    }

    uint32_t next;
    if (keep) {
        if (s_filled < TRACE_NUM_SLOTS) s_filled++;
        next = pick_victim();            // empty slot, or the fastest to evict
        if (next == trace_stage)         // never evict the frame we just kept
            next = (next + 1u) % TRACE_NUM_SLOTS;
    } else {
        next = trace_stage;              // discard: reuse this slot
    }

    trace_slot_t *ns = &trace_slots[next];
    ns->count = 0;
    ns->truncated = 0;
    ns->dur_cyc = 0;
    ns->frame_no = ++s_frame_no;
    trace_stage = next;
    s_stage_start = cyc ? cyc : 1u;
}

void trace_init(void)
{
    // Enable DWT cycle counter: DEMCR.TRCENA then DWT_CTRL.CYCCNTENA.
    *(volatile uint32_t *)0xE000EDFC |= (1u << 24);
    *(volatile uint32_t *)0xE0001000 |= 1u;

    for (uint32_t i = 0; i < TRACE_NUM_SLOTS; i++) {
        trace_slots[i].count = 0;
        trace_slots[i].truncated = 0;
        trace_slots[i].dur_cyc = 0;
        trace_slots[i].frame_no = 0;
    }
    trace_stage = 0;
    s_frame_no = 0;
    s_stage_start = 0;
    s_filled = 0;
    TRACE_EVT(TEV_FRAME_MARK, 0);        // opens the first frame
}

#endif
