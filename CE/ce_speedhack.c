/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */

#include <string.h>

#include "snes9x.h"
#include "memmap.h"

#include "ce_log.h"
#include "ce_speedhack.h"

/* The core's asm Op42 (src/os9x_65c816_opcodes.S) decodes "42 XY" as:
 * set CPU cycles to NextEvent, then execute the branch whose opcode is
 * X0 with the sign-extended 8-bit offset FY. So a two-byte relative
 * branch "X0 FY" at the top of an idle loop can be rewritten in place
 * as "42 XY". Only the nine branch opcodes the asm jump table handles
 * are valid, and the offset must be in -16..-1 (0xF0..0xFF).
 *
 * Addresses and original bytes are the main-CPU entries from the
 * snes9x_3ds port's CMemory::ApplyROMFixes (its SpeedHackAdd() table),
 * restricted to ones that fit this encoding:
 *   - Axelay (F0 DB) is left out: offset -37 cannot be encoded.
 *   - SA-1 titles are left out entirely: the SA-1 build of Op42 is an
 *     empty stub (src/cpuops.c, whole body under #ifndef SA1_OPCODES),
 *     and skipping main-CPU cycles would also starve the SA-1, which the
 *     core steps from the main CPU loop (snes9x_3ds disables its skip
 *     while SA1.Executing for the same reason).
 * The original bytes are verified before patching, so a different
 * revision/region that happens to share the internal name is left
 * untouched. */
typedef struct
{
   const char *romName;
   uint32      address;   /* SNES bank:address of the branch opcode */
   uint8       opcode;    /* expected original branch opcode */
   uint8       offset;    /* expected original branch offset */
} CeSpeedHack;

static const CeSpeedHack kSpeedHacks[] =
{
   { "YOSHI'S ISLAND",                     0x0080F4, 0x30, 0xFB }, /* US + EUR */
   { "SUPER MARIO KART",                   0x80805E, 0xF0, 0xFC }, /* US + EUR */
   { "F-ZERO",                             0x00803C, 0xF0, 0xFC }, /* US + EUR */
   { "\xb4\xb0\xbd\xa6\xc8\xd7\xb4\x21",   0x80C458, 0x10, 0xFB }, /* Ace o Nerae! */
};

#define CE_SPEEDHACK_COUNT (sizeof(kSpeedHacks) / sizeof(kSpeedHacks[0]))

static int IsEncodableBranch(uint8 opcode)
{
   switch (opcode)
   {
   case 0x10: case 0x30: case 0x50: case 0x70: case 0x80:
   case 0x90: case 0xB0: case 0xD0: case 0xF0:
      return 1;
   }
   return 0;
}

/* Same lookup the core's S9xGetMemPointer() (src/getset.h, static
 * inline there so it can't be linked to) does for ROM/RAM blocks: a
 * Map[] entry below MAP_LAST is a special handler (registers, SRAM,
 * chips), not a direct pointer. */
static uint8 *RomPointer(uint32 address)
{
   uint8 *block = Memory.Map[(address >> MEMMAP_SHIFT) & MEMMAP_MASK];

   if (block < (uint8 *)MAP_LAST)
      return NULL;
   return block + (address & 0xffff);
}

int CeSpeedHackApply(void)
{
   unsigned i;
   int applied = 0;

   if (Settings.SA1)
      return 0;

   for (i = 0; i < CE_SPEEDHACK_COUNT; i++)
   {
      const CeSpeedHack *h = &kSpeedHacks[i];
      uint8 *p;

      if (strcmp(Memory.ROMName, h->romName) != 0)
         continue;
      if (!IsEncodableBranch(h->opcode) || h->offset < 0xF0)
         continue;

      p = RomPointer(h->address);
      if (!p)
      {
         CeLog("CeSpeedHackApply: %06X not mapped to ROM, skipped", (unsigned)h->address);
         continue;
      }
      if (p[0] != h->opcode || p[1] != h->offset)
      {
         CeLog("CeSpeedHackApply: %06X bytes %02X %02X != expected %02X %02X, skipped",
               (unsigned)h->address, p[0], p[1], h->opcode, h->offset);
         continue;
      }

      p[0] = 0x42;
      p[1] = (uint8)((h->opcode & 0xF0) | (h->offset & 0x0F));
      applied++;
      CeLog("CeSpeedHackApply: patched %06X -> 42 %02X", (unsigned)h->address, p[1]);
   }

   return applied;
}
