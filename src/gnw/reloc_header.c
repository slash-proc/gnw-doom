//
// Relocation header at offset 0 of the app image (before .text.entry).
//
// Lets the firmware launch the app from an arbitrary extflash address: it
// reads this header at the install address X, relocates the image (rewrites
// the absolute pointers in the reloc table by X - link_base), and jumps to
// X + entry_offset. The build tool (scripts/build/gen_reloc_table.py) fills
// link_base/entry_offset/image_size/reloc_offset/reloc_count after link.
//
// Layout (8 x u32 = 32 bytes; doom_entry follows immediately):
//   [0] magic 'DXR1'   [1] link_base       [2] entry_offset
//   [3] image_size     [4] reloc_offset    [5] reloc_count   [6][7] pad
//

#include <stdint.h>

#define RELOC_HDR_MAGIC 0x31525844u  // "DXR1"

__attribute__((section(".reloc_hdr"), used))
const uint32_t reloc_hdr[8] = { RELOC_HDR_MAGIC, 0, 0, 0, 0, 0, 0, 0 };
