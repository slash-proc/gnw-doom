//
// gwhb_entry.c — stage-1 copier of the GWHB RAM overlay.
//
// retro-go's homebrew launcher copies the whole image to __RAM_EMU_START__
// (0x2404B000) and jumps here (image offset 4, after the 'GWHB' magic). We
// execute IN PLACE at the load address, on the firmware's stack, and unpack
// the image: every section was linked with its LMA in the image (BLOB region)
// and its VMA at its final home (ITCM / DTCM / top of AXISRAM). Copy each out,
// make the copied code visible to instruction fetch, then enter doom_start —
// which lives in .text_axis and from there owns the machine (zeroing .bss,
// which overlaps and thus retires this code and the rest of the image).
//
// Keep this file freestanding and self-contained: no globals, no libc, no
// calls outside .gwhb except the final jump. The linker places ONLY this
// object's code/rodata in .gwhb.
//

#include <stdint.h>

extern uint32_t __itcram_hot_start__[], __itcram_hot_end__[], __itcram_hot_lma[];
extern uint32_t _doom_data_vma_start[], _doom_data_vma_end[], _doom_data_lma[];
extern uint32_t __text_dtcm_start__[], __text_dtcm_end__[], __text_dtcm_lma[];
extern uint32_t __text_axis_start__[], __text_axis_end__[], __text_axis_lma[];

extern void doom_start(uint8_t load_state, uint8_t start_paused, int8_t save_slot);

static inline void copy_words(uint32_t *dst, const uint32_t *end, const uint32_t *src)
{
    while (dst < end)
        *dst++ = *src++;
}

__attribute__((section(".gwhb_entry"), used))
void gwhb_entry(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    copy_words(__itcram_hot_start__, __itcram_hot_end__, __itcram_hot_lma);
    copy_words(_doom_data_vma_start, _doom_data_vma_end, _doom_data_lma);
    copy_words(__text_dtcm_start__, __text_dtcm_end__, __text_dtcm_lma);
    copy_words(__text_axis_start__, __text_axis_end__, __text_axis_lma);

    // Copied code is fetched from AXI/AHB behind the D-cache; clean those
    // lines to memory, then drop the I-cache. (ITCM is tightly coupled — no
    // maintenance needed. The AHB ranges are MPU-uncached under retro-go's
    // boot MPU config but cacheable under the default map — clean them too
    // so the same image is correct under either.)
    __asm__ volatile ("dsb");
    for (uintptr_t a = (uintptr_t)__text_axis_start__ & ~31u;
         a < (uintptr_t)__text_axis_end__; a += 32)
        *(volatile uint32_t *)0xE000EF68 = a;   /* DCCMVAC: clean by MVA */
    for (uintptr_t a = (uintptr_t)__text_dtcm_start__ & ~31u;
         a < (uintptr_t)__text_dtcm_end__; a += 32)
        *(volatile uint32_t *)0xE000EF68 = a;
    __asm__ volatile ("dsb");
    *(volatile uint32_t *)0xE000EF50 = 0;       /* ICIALLU: kill I-cache  */
    __asm__ volatile ("dsb; isb");

    doom_start(load_state, start_paused, save_slot);
}
