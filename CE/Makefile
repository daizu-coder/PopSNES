# SPDX-License-Identifier: MIT
# Copyright (c) 2026 daizu-coder
#
# Windows CE (SHARP Brain PW-G5200, ARM, no HW integer divide) port of
# snes9x2002 via its libretro core. Built with the cegcc cross toolchain.
#
# Run from inside this directory:
#   make clean && make && make strip

CORE_DIR          := ../src
LIBRETRO_DIR      := ../libretro
LIBRETRO_COMM_DIR := $(LIBRETRO_DIR)/libretro-common

# Use the real ARM assembly CPU/APU interpreter cores and the ARM-tuned
# C renderer set (not the portable plain-C fallback) - this port exists
# specifically to get away from an earlier plain-C-only (WSNES9X-based) prototype's
# video/audio contention slowdown, so the asm paths are load-bearing here.
ARM_ASM    := 1
ASM_CPU    := 1
ASM_SPC700 := 1

include ../Makefile.common

TOOLPREFIX := /opt/cegcc/bin/arm-mingw32ce-
CC      := $(TOOLPREFIX)gcc
STRIP   := $(TOOLPREFIX)strip
OBJDUMP := $(TOOLPREFIX)objdump
NM      := $(TOOLPREFIX)nm
WINDRES := $(TOOLPREFIX)windres

# armv5te: matches the target device (has BLX/BLX-reg and other v5T
# instructions the asm cores use, but no hardware integer divide).
# mno-unaligned-access: the asm/C cores assume natural alignment isn't
# guaranteed to be exploitable; let the compiler emit safe byte/half
# accesses rather than faulting on ARM's strict alignment trap.
ARCH_FLAGS := -march=armv5te -mno-unaligned-access

SOURCES_CE := ce_main.c ce_log.c ce_display.c ce_input.c ce_audio.c ce_video.c ce_config.c ce_lang.c ce_fileopen.c ce_bmpfont.c ce_speedhack.c

DEFINES  += -DWINCE
INCLUDES += -I. -Icompat

# -O3: an earlier prototype (WSNES9X-based) measured a real (if modest) gain over -O2
# on this device with no regressions; -flto is NOT used - this cegcc
# ld doesn't support LTO plugin objects (confirmed by that prototype).
CFLAGS  := $(ARCH_FLAGS) -O3 -fomit-frame-pointer $(DEFINES) $(COMMON_DEFINES) $(INCLUDES)
# No C++ anywhere in this port (core is all .c, CE frontend is written in
# plain C on purpose) - avoids ever depending on libstdc++-6.dll, which
# does not exist on this device and silently kills startup if linked in.
# aygshell.dll is not linked (and its LoadLibrary-based use has been
# removed from the CE frontend entirely, see ce_main.c) - it's a
# device-bundled file, not guaranteed on every Windows CE target.
# commdlg (GetOpenFileNameW) is no longer linked either - the ROM picker
# is a self-drawn dialog now (ce_fileopen.c), not the common file-open
# dialog; see ce_fileopen.c's header comment for why.
# Video output is plain GDI (ce_display.c - CreateDIBSection/BitBlt, all
# in coredll) - an earlier GAPI-based backend ce_gapi.c was removed; see
# ce_display.c's header comment for the licensing-caution reason.
LDFLAGS := -static-libgcc -lm

RESOURCES := ce_res.o

OBJECTS := $(SOURCES:.c=.o) $(SOURCES_ASM:.S=.o) $(SOURCES_CE:.c=.o) $(RESOURCES)

TARGET := AppMain.exe

# UI bitmap font (ce_bmpfont.c) - same switch as PopPCE (NitroGrafx-main,
# 2026-09-27). Galmuri14 is the default; the others stay buildable:
#   make                     Galmuri14 (default)  -> AppMain.exe
#   make CE_FONT=shinonome   Shinonome 16dot      -> AppMain_shinonome.exe
#   make CE_FONT=galmuri11   GalmuriMono11        -> AppMain_galmuri11.exe
# ce_galmuri14.h / ce_galmuri11.h are copies of PopPCE's
# tools/galmuri2c.py output. Only ce_bmpfont.o and ce_fileopen.o see the
# define; the .font-<name> stamp makes them rebuild when CE_FONT changes.
CE_FONT ?= galmuri14
ifeq ($(CE_FONT),galmuri14)
FONT_DEFS := -DCE_FONT_GALMURI14
else ifeq ($(CE_FONT),shinonome)
FONT_DEFS :=
TARGET := AppMain_shinonome.exe
else ifeq ($(CE_FONT),galmuri11)
FONT_DEFS := -DCE_FONT_GALMURI11
TARGET := AppMain_galmuri11.exe
else
$(error CE_FONT must be galmuri14, shinonome or galmuri11)
endif
FONT_STAMP := .font-$(CE_FONT)

.PHONY: all clean strip

all: $(TARGET)

$(FONT_STAMP):
	rm -f .font-*
	touch $@

ce_bmpfont.o ce_fileopen.o: $(FONT_STAMP) ce_bmpfont.h ce_galmuri14.h ce_galmuri11.h ce_shinonome16.h
ce_bmpfont.o ce_fileopen.o: CFLAGS += $(FONT_DEFS)

$(TARGET): $(OBJECTS)
	$(CC) -o $@ $(OBJECTS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.rc
	$(WINDRES) -F pe-arm-wince-little -o $@ $<

# ce_res.o also has to be rebuilt when the resource IDs, the embedded app
# icon or the main-menu mascot bitmap (both in CE/icon/) change.
ce_res.o: ce_resource.h icon/popsnes.ico icon/popsnes_mascot.bmp

strip: all
	$(STRIP) $(TARGET)

clean:
	rm -f .font-* AppMain_shinonome.exe AppMain_galmuri11.exe
	rm -f $(OBJECTS) $(TARGET)
