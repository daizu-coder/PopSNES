/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */

#ifndef CE_SPEEDHACK_H
#define CE_SPEEDHACK_H

/* Per-game idle-loop speed hacks. Call once right after a successful
 * retro_load_game(): patches the loaded ROM image in memory so the
 * core's ARM asm Op42 ("42 XY", the SNESAdvance speed-hack opcode that
 * src/os9x_65c816_opcodes.S already implements but nothing ever
 * patched in) fast-forwards the main CPU to the next event instead of
 * spinning in a wait loop. Returns the number of patches applied. */
int CeSpeedHackApply(void);

#endif
