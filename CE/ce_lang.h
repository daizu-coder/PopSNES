/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Japanese/English UI text toggle - a persisted on/off flag consulted by
 * every dialog's own ApplyXLanguage() function (ce_main.c/ce_input.c/
 * ce_audio.c/ce_video.c/ce_fileopen.c) plus the Shinonome bitmap-font
 * renderer (ce_bmpfont.c/.h, CE/ce_shinonome16.h) that actually draws the
 * text.
 *
 * This file used to also load a separate TrueType font file -
 * AddFontResourceW() on a background thread (ported from an
 * earlier prototype), since removed: shipping a separate font file
 * alongside the binary is no longer needed, and AddFontResource()/
 * RemoveFontResource() were the single biggest source of real-hardware
 * hangs and glyph corruption across the sibling CE ports (see the dev notes rounds 17-21 and the sister QuickNES
 * CE / gnuboy CE dev notes files). It's replaced by CeBmpFontDrawTextW()
 * (ce_bmpfont.c) - a Shinonome bitmap font baked into the binary at build
 * time instead of loaded from a file at runtime, so there's no
 * load-failure case to guard against and no font resource to leak across
 * relaunches any more. See CE/THIRDPARTY_LICENSES.txt for the font's
 * license text and author credit.
 *
 * The old TrueType-era API surface (CeLangGetUIFont(),
 * CeLangFontLoadFailed(), CeLangShutdown()) is gone along with it - the
 * bitmap font never fails to load and owns no HFONT/thread to release,
 * so every ApplyXLanguage() now just calls CeBmpFontDrawTextW()/
 * CeBmpFontDrawOwnerButton()/CeBmpFontPaintLabel() directly (see
 * ce_bmpfont.h) instead of fetching a font handle first.
 */
#ifndef CE_LANG_H
#define CE_LANG_H

/* Loads the persisted UILanguageJapanese flag from CeConfigLoad()'s
 * table - call once from WinMain, after CeConfigLoad(). */
void CeLangInit(void);

/* Persisted preference (CeConfigLoad()'s table, key "UILanguageJapanese"),
 * default Japanese (1). Originally defaulted to English (0) as an opt-in
 * layered onto an English-only UI, hedged against the TrueType font failing
 * to load; the bitmap-font port removed that failure mode, and the
 * device's homebrew audience is Japanese, so the default was flipped to
 * Japanese (round 32). Users switch to English via Video Config's
 * language stepper, which writes UILanguageJapanese=0. */
int  CeLangIsJapanese(void);
void CeLangSetJapanese(int japanese);

#endif
