/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Video Config: scale mode (1:1 vs stretch-to-fill), transparency
 * effects, and frame skip. Scale mode is consumed directly by
 * ce_display.c's blit; transparency/frame skip are core-side settings
 * exposed through the standard libretro core-options environment calls
 * (see ce_main.c's ce_environment) rather than by reaching into the
 * core's Settings struct directly, matching this port's libretro-
 * frontend-only architecture.
 */
#ifndef CE_VIDEO_H
#define CE_VIDEO_H

#include <windows.h>

typedef enum
{
    CE_SCALE_1TO1        = 0,
    /* Stretch to the full physical display, both axes independently
     * (the "Full" Scale choice). For the SNES's 256-wide frame on this
     * 480-wide device that is an exact 15/8, so ce_display.c's blit
     * takes a 16-source/30-dest 32-bit-packed fast path for it (round
     * 26) rather than the generic DDA scaler. */
    CE_SCALE_EXPAND      = 1,
    /* Fill the display top-to-bottom; horizontal scale is a FIXED 3/2
     * ratio (the "x1.5" Scale choice), letterbox bars left/right. Not
     * derived from display/source height: the fixed ratio is what lets
     * ce_display.c's blit take its 4-source/6-dest 32-bit-packed fast
     * path. For the SNES's 256-wide frame this is 256 -> 384, close to
     * aspect-correct (which would be ~365) and within the 480 screen. */
    CE_SCALE_FULLSCREEN  = 2,
    /* Same vertical fill as Full Screen, horizontal scale a FIXED 7/4
     * ratio (the "Wide" Scale choice) - between Full Screen's 3/2 and
     * Expand's full-width stretch. 256 -> 448; ce_display.c's blit
     * takes an 8-source/14-dest 32-bit-packed fast path for it. */
    CE_SCALE_HALFSTRETCH = 3,
} CeScaleMode;

/* Loads any saved settings from the registry (falls back to built-in
 * defaults). Call once from WinMain before retro_init(). */
void CeVideoInit(void);

void CeShowVideoConfigDialog(HWND owner);

/* Consulted directly by ce_display.c's CeDisplayBlitRGB565 every frame. */
CeScaleMode CeVideoGetScaleMode(void);

/* ce_main.c's ce_environment() calls this for RETRO_ENVIRONMENT_GET_VARIABLE:
 * if key is one this module owns (snes9x2002_transparency,
 * snes9x2002_frameskip, snes9x2002_frameskip_interval), fills *outValue
 * with a string valid until the next call and returns 1; otherwise
 * returns 0 and leaves *outValue untouched (falls through to the core's
 * own built-in default for that option). */
int CeVideoEnvGetVariable(const char *key, const char **outValue);

/* ce_main.c's ce_environment() calls this for
 * RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: returns 1 (and clears the flag)
 * exactly once after CeShowVideoConfigDialog's OK handler changes
 * transparency/frame skip, so the core picks the new value up on the
 * very next retro_run() without needing a fresh retro_load_game(). */
int CeVideoConsumeDirty(void);

/* Automatic frame skip (round 10; range widened to 0..30 in round 11 to
 * match the core's own cap): Video Config's Frame Skip dial (0..30) is
 * the *maximum* number of consecutive frames the core is allowed to
 * skip while it's actually falling behind (snes9x2002_frameskip "auto" -
 * the core's own audio-buffer-underrun signal), not a fixed always-
 * skip-N cadence. The core's own internal safety cap on consecutive
 * skips (FRAMESKIP_MAX in libretro/libretro.c) is a fixed 30 and isn't
 * exposed as a per-port option, so ce_main.c's WinMain loop enforces
 * this dial's own (possibly smaller) cap itself using the pair below:
 *
 *   - CeVideoFrameSkipShouldForceRender() is checked once per frame,
 *     right before WinMain would otherwise forward CeAudioGetBufferStatus()'s
 *     real reading to the core's audio-buffer-status callback (only
 *     relevant while g_audioBuffStatusCb is non-NULL, i.e. Frame Skip >
 *     0). Returns 1 once the cap has been hit, telling the caller to
 *     report "buffer not active" instead of the real reading for this
 *     one frame - the core's FRAMESKIP_AUTO logic never skips a frame
 *     it's told the buffer isn't active for, so this forces a render.
 *   - CeVideoFrameSkipNotifyRendered() is called exactly once per frame
 *     by ce_video_refresh() (rendered = data != NULL) to keep the
 *     consecutive-skip counter behind ShouldForceRender() in sync with
 *     what the core actually did.
 */
int CeVideoFrameSkipShouldForceRender(void);
void CeVideoFrameSkipNotifyRendered(int rendered);

#endif
