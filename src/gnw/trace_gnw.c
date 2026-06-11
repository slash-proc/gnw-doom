//
// Pipeline tracer buffer + DWT cycle counter init. See trace_gnw.h.
//
#include "trace_gnw.h"

#if DOOMX_TRACE

// Lives in the dedicated .trace_buf section: linker carves it from the top
// of AXISRAM (the zone shrinks by 512 KB; gameplay uses <40 KB of it).
__attribute__((section(".trace_buf"))) trace_entry_t trace_buf[TRACE_ENTRIES];
volatile uint32_t trace_head;

void trace_init(void)
{
    // Enable DWT cycle counter: DEMCR.TRCENA then DWT_CTRL.CYCCNTENA.
    *(volatile uint32_t *)0xE000EDFC |= (1u << 24);
    *(volatile uint32_t *)0xE0001000 |= 1u;
    trace_head = 0;
    TRACE_EVT(TEV_FRAME_MARK, 0);
}

#endif
