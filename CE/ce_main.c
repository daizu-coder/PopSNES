/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */

/*
 * Windows CE frontend for snes9x2002 (SHARP Brain PW-G5200, ARM).
 *
 * This is a *libretro frontend*, not a port that reaches into the S9x
 * core directly: ../libretro/libretro.c already implements every port
 * hook the core needs (S9xReadJoypad, S9xDeinitUpdate, buffer setup,
 * etc.) in terms of the five retro_set_* callbacks below. All we do here
 * is drive retro_init/retro_load_game/retro_run and turn those callbacks
 * into real GDI/waveOut/key I/O.
 *
 * Video output is plain GDI (ce_display.c - CreateDIBSection + BitBlt),
 * not GAPI; an earlier GAPI-based backend was removed. See
 * ce_display.c's header comment for the licensing-caution reason.
 *
 * UI shape: a real Win32 menu (File/Input/Sound/Video/Help), touch-to-
 * reveal - shown at startup (no ROM loaded, black client area, no frame
 * blitted yet) and whenever the screen is tapped mid-game (pauses
 * emulation; the menu dialog just draws on top of the game window, see
 * ShowMainMenuDialog()); hidden again once a game is running.
 *
 * Milestone status: File>Open/Exit, the menu shell, Input Config
 * (ce_input.c), Sound Config + audio output (ce_audio.c), save states,
 * and Video Config (ce_video.c) are all implemented.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>

#include <libretro.h>

#include "ce_log.h"
#include "ce_display.h"
#include "ce_input.h"
#include "ce_audio.h"
#include "ce_video.h"
#include "ce_config.h"
#include "ce_lang.h"
#include "ce_fileopen.h"
#include "ce_resource.h"
#include "ce_bmpfont.h"
#include "ce_speedhack.h"

static const wchar_t kWndClassName[] = L"PopSNESWnd";
static const wchar_t kMutexName[]    = L"PopSNES_SingleInstance";

static HWND   g_hwnd     = NULL;
static HANDLE g_mutex    = NULL;
static volatile int g_running = 0;

static void *g_romData = NULL;
static long  g_romSize = 0;
static wchar_t g_romPath[MAX_PATH] = L""; /* last successfully-loaded ROM's path, for save-state file naming */

/* Last frame the core actually rendered (ce_video_refresh), for the main
 * menu's Screenshot button. Points into the core's GFX.Screen, which
 * libretro.c allocates once in retro_init and never moves, and which
 * nothing overwrites while the menu has the game paused. Cleared on
 * every ROM load so a new game can't save the previous one's frame. */
static const void *g_lastFrame = NULL;
static unsigned g_lastFrameW = 0, g_lastFrameH = 0;
static size_t g_lastFramePitch = 0;

/* g_romLoaded: a game has been successfully retro_load_game()'d at
 * least once (stays true across File>Open reloads until exit).
 * g_paused: the touch-to-reveal menu is up right now - retro_run() is
 * not called while this is true, and CeDisplaySuspend() has released
 * the cached window DC so the menu dialog and WM_PAINT own the screen. */
static int  g_romLoaded = 0;
static int  g_paused    = 0;
/* Frame pacing: length of one emulated frame in microseconds, from the
 * loaded ROM's retro_system_av_info (NTSC ~16639us, PAL ~19873us).
 * 0 = no ROM yet. g_paceDeadline is reset to "now" whenever the loop
 * falls behind (menu, slow frames) so it never tries to catch up. */
static unsigned g_framePeriodUs = 0;
static int      g_paceResync    = 1;

static void CeShutdown(int exitCode); /* used by MainMenuDlgProc, below */
static void CeShowShellChrome(HWND hwnd); /* used by CeShutdown, defined further below */
static void CeHideShellChrome(HWND hwnd); /* used by ShowMainMenuDialog and WinMain, defined further below */
static void CeShowMsgBox(HWND owner, const wchar_t *text); /* used by CeSaveState/CeLoadState, defined further below */

/* ------------------------------------------------------------------ */
/* libretro callbacks                                                  */
/* ------------------------------------------------------------------ */

/* Set by the core via RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK
 * whenever Video Config's Frame Skip is above 0 (ce_video.c) - NULL
 * otherwise (Frame Skip Off, or before retro_load_game()'s first
 * check_variables() call). Invoked once per frame from WinMain's main
 * loop, right before retro_run(), per that environment call's contract -
 * see the call site below. */
static retro_audio_buffer_status_callback_t g_audioBuffStatusCb = NULL;

static bool ce_environment(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    {
        enum retro_pixel_format *fmt = (enum retro_pixel_format *)data;
        /* ce_display.c's off-screen DIB section is RGB565 (BI_BITFIELDS
         * 0xF800/0x07E0/0x001F); refuse anything else so the core
         * doesn't silently assume a format we can't display. */
        return (*fmt == RETRO_PIXEL_FORMAT_RGB565);
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        /* Video Config's transparency/frame-skip settings (ce_video.c)
         * ride the core's own existing core-options protocol
         * (snes9x2002_transparency / snes9x2002_frameskip* - see
         * check_variables() in libretro/libretro.c) instead of a new
         * side channel - CE just needs to answer these two queries. */
        struct retro_variable *var = (struct retro_variable *)data;
        return CeVideoEnvGetVariable(var->key, &var->value) ? true : false;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        /* Lets a Video Config change made mid-session (ROM already
         * loaded, retro_load_game() - which would otherwise be the only
         * point check_variables() re-reads these - not called again)
         * take effect on the very next retro_run() instead of needing a
         * File>Open reload. */
        *(bool *)data = CeVideoConsumeDirty() ? true : false;
        return true;

    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK:
    {
        /* The core only asks for this when frame skip is "auto" (see
         * check_variables()/retro_set_audio_buff_status_cb() in
         * libretro/libretro.c) - without answering it, that mode
         * silently never skips anything (retro_audio_buff_active stays
         * false forever), which is exactly the trap Video Config's
         * automatic Frame Skip (ce_video.c, added after the user
         * reported uneven speed with a fixed always-skip-N cadence,
         * 2026-08-01 round 7) would otherwise fall into. data is NULL
         * when the core wants to unregister. */
        const struct retro_audio_buffer_status_callback *cb =
            (const struct retro_audio_buffer_status_callback *)data;
        g_audioBuffStatusCb = cb ? cb->callback : NULL;
        return true;
    }

    default:
        /* Everything else (GET_OVERSCAN, GET_INPUT_BITMASKS, ...) is
         * optional per the libretro API contract - returning false tells
         * the core to use its built-in defaults, which is exactly what
         * we want until we wire up our own settings UI. */
        return false;
    }
}

static void ce_video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
    /* Keeps Video Config's Frame Skip consecutive-skip counter (round
     * 10 - see ce_video.h) in sync with what the core actually did this
     * frame: data is NULL exactly when the core skipped rendering. */
    CeVideoFrameSkipNotifyRendered(data != NULL);

    if (!data)
        return; /* duplicate/skipped frame - nothing new to draw */

    g_lastFrame = data;
    g_lastFrameW = width;
    g_lastFrameH = height;
    g_lastFramePitch = pitch;

    /* Self-contained per call (width/height/pitch given fresh every
     * time) - no need to track GFX.Pitch internals like the old
     * Win32.cpp-based port had to. */
    CeDisplayBlitRGB565(data, width, height, (unsigned)pitch);
}

static void ce_audio_sample_noop(int16_t left, int16_t right)
{
    /* The core only ever uses retro_set_audio_sample_batch (see
     * retro_set_audio_sample() in libretro.c - it's an intentional
     * no-op setter), but we still register a real callback rather than
     * NULL to avoid relying on that being true forever. */
    (void)left;
    (void)right;
}

static size_t ce_audio_sample_batch(const int16_t *data, size_t frames)
{
    return CeAudioPushSamples(data, frames);
}

static void ce_input_poll(void)
{
    CeInputPoll();
}

static int16_t ce_input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    return CeInputState(port, device, index, id);
}

/* ------------------------------------------------------------------ */
/* ROM loading                                                         */
/* ------------------------------------------------------------------ */

static int PickAndLoadRom(HWND owner, wchar_t *outPath, size_t outPathCount)
{
    FILE *f;

    /* Free any previously-loaded ROM buffer up front, so this is safe
     * to call again for File>Open while a game is already running (the
     * caller is responsible for retro_unload_game()'ing the old game
     * first - this just avoids leaking/losing track of the old malloc). */
    if (g_romData)
    {
        free(g_romData);
        g_romData = NULL;
        g_romSize = 0;
    }

    memset(outPath, 0, outPathCount * sizeof(wchar_t));

    /* Custom listbox-based picker (ce_fileopen.c), not GetOpenFileNameW()
     * - the standard common dialog has no way to render Japanese folder/
     * file names on this device (see ce_fileopen.c's header comment). */
    if (!CeShowFileOpenDialog(owner, outPath, outPathCount))
    {
        CeLog("PickAndLoadRom: file picker cancelled");
        return 0;
    }

    f = _wfopen(outPath, L"rb");
    if (!f)
    {
        CeLog("PickAndLoadRom: failed to open selected file");
        return 0;
    }

    fseek(f, 0, SEEK_END);
    g_romSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (g_romSize <= 0)
    {
        fclose(f);
        CeLog("PickAndLoadRom: empty or unreadable file (size=%ld)", g_romSize);
        return 0;
    }

    g_romData = malloc((size_t)g_romSize);
    if (!g_romData)
    {
        fclose(f);
        CeLog("PickAndLoadRom: malloc(%ld) failed", g_romSize);
        return 0;
    }

    if (fread(g_romData, 1, (size_t)g_romSize, f) != (size_t)g_romSize)
    {
        fclose(f);
        free(g_romData);
        g_romData = NULL;
        CeLog("PickAndLoadRom: short read");
        return 0;
    }

    fclose(f);
    CeLog("PickAndLoadRom: loaded %ld bytes", g_romSize);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Battery-backed cartridge save (SRAM)                                */
/* ------------------------------------------------------------------ */

/* Loads "<romPath>.srm" into the core's SRAM, if this game has any
 * (RETRO_MEMORY_SAVE_RAM) and a save file already exists. This is what
 * makes a game's own in-cartridge save feature (its own menu's "Save" -
 * not this frontend's separate Save State snapshot, see CeSaveState/
 * CeLoadState above) survive across app restarts, same as a real
 * battery-backed cartridge would (user request, 2026-08-01 round 8 -
 * covers "temporary save" cartridge variants too, since those still go
 * through the same SRAM the core exposes here, the core has no separate
 * concept of "temporary" vs. "permanent" SRAM). No .srm file yet is the
 * normal case for a new game (or one with no SRAM at all) and isn't
 * logged as an error. */
static void CeLoadSram(void)
{
    void *sram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    wchar_t sramPath[MAX_PATH + 8];
    FILE *f;
    size_t got;

    if (!sram || size == 0)
        return; /* this game has no battery-backed SRAM */

    _snwprintf(sramPath, MAX_PATH + 8, L"%s.srm", g_romPath);
    f = _wfopen(sramPath, L"rb");
    if (!f)
    {
        CeLog("CeLoadSram: no .srm file yet (new game, or none saved)");
        return;
    }

    /* Read at most `size` bytes - a mismatched-size .srm (shouldn't
     * happen for a given ROM, but don't overrun the core's buffer if it
     * somehow does) is truncated, not rejected outright. */
    got = fread(sram, 1, size, f);
    fclose(f);
    CeLog("CeLoadSram: loaded %lu of %lu bytes", (unsigned long)got, (unsigned long)size);
}

/* Writes the core's current SRAM out to "<romPath>.srm" - the other half
 * of CeLoadSram(). Called whenever a loaded game's SRAM is about to stop
 * being the live one (File>Open loading a different ROM, or app exit),
 * so an in-game save made this session isn't lost. No periodic/crash-
 * safe autosave beyond these two points - not asked for, and this
 * device's actual power-loss risk profile is unknown. */
static void CeSaveSram(void)
{
    void *sram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    wchar_t sramPath[MAX_PATH + 8];
    FILE *f;

    if (!sram || size == 0)
        return; /* this game has no battery-backed SRAM - nothing to save */

    _snwprintf(sramPath, MAX_PATH + 8, L"%s.srm", g_romPath);
    f = _wfopen(sramPath, L"wb");
    if (!f)
    {
        CeLog("CeSaveSram: failed to open .srm file for write");
        return;
    }

    fwrite(sram, 1, size, f);
    fclose(f);
    CeLog("CeSaveSram: saved %lu bytes", (unsigned long)size);
}

/* Picks + reads a ROM (via PickAndLoadRom) and hands it to the core.
 * Safe to call both for the first load and for File>Open while a game
 * is already running (unloads the previous game first). Returns 1 on
 * success, 0 if the user cancelled the picker or loading failed - in
 * both failure cases whatever was running before is left untouched. */
static int LoadRomFlow(HWND hwnd)
{
    wchar_t romPath[MAX_PATH];
    struct retro_game_info game;
    struct retro_system_av_info avInfo;
    static char pathUtf8[MAX_PATH];
    wchar_t title[MAX_PATH + 32];
    wchar_t *base;

    if (!PickAndLoadRom(hwnd, romPath, MAX_PATH))
        return 0; /* cancelled/failed - PickAndLoadRom already logged why */

    if (g_romLoaded)
    {
        CeSaveSram(); /* g_romPath/the core's SRAM still refer to the *previous* game here - new one isn't loaded yet */
        retro_unload_game();
    }

    /* game.path is never dereferenced by this core - retro_load_game()
     * (libretro/libretro.c) only touches game->data/game->size
     * (S9xSetStreamBuffer(), ROM already fully read into g_romData by
     * PickAndLoadRom()'s _wfopen()) - grep over the whole core tree
     * confirms no other reference to it. This CP_ACP conversion can't
     * represent a Japanese path losslessly, but since nothing ever reads
     * the result, that was never actually a problem: the ASCII-only
     * pick guard this port carried (mirrored from the sister PopSG/
     * PicoDrive CE ports, whose cores *do* reopen game.path themselves)
     * was protecting against a failure mode that can't happen here, and
     * has been removed. */
    WideCharToMultiByte(CP_ACP, 0, romPath, -1, pathUtf8, MAX_PATH, NULL, NULL);
    memset(&game, 0, sizeof(game));
    game.path = pathUtf8;
    game.data = g_romData;
    game.size = (size_t)g_romSize;

    if (!retro_load_game(&game))
    {
        CeLog("LoadRomFlow: retro_load_game failed");
        MessageBoxW(hwnd, L"Failed to load ROM.", L"PopSNES", MB_OK);
        g_romLoaded = 0;
        return 0;
    }

    g_romLoaded = 1;
    g_lastFrame = NULL;
    wcsncpy(g_romPath, romPath, MAX_PATH - 1);
    g_romPath[MAX_PATH - 1] = L'\0';

    CeLoadSram();
    CeSpeedHackApply(); /* before the first retro_run(); logs what it patched */

    retro_get_system_av_info(&avInfo);
    CeLog("LoadRomFlow: loaded, geometry=%ux%u fps=%.3f sample_rate=%.0f",
          avInfo.geometry.base_width, avInfo.geometry.base_height,
          avInfo.timing.fps, avInfo.timing.sample_rate);
    CeAudioStart(avInfo.timing.sample_rate);
    g_framePeriodUs = (avInfo.timing.fps > 1.0)
        ? (unsigned)(1000000.0 / avInfo.timing.fps) : 16639u;
    g_paceResync = 1;

    base = wcsrchr(romPath, L'\\');
    _snwprintf(title, MAX_PATH + 32, L"PopSNES - %s", base ? base + 1 : romPath);
    SetWindowTextW(g_hwnd, title); /* always the main window, even when
                                     * called with a dialog as `hwnd` */

    return 1;
}

/* ------------------------------------------------------------------ */
/* Save state (single slot per ROM: "<romPath>.state")                */
/* ------------------------------------------------------------------ */

/* Returns 1 on success, 0 on failure. Shows no UI itself - the caller
 * (SaveStateDlgProc) folds the result into its own single dialog so no
 * second modal is ever nested. */
static int CeSaveState(void)
{
    size_t size;
    void *buffer;
    wchar_t statePath[MAX_PATH + 8];
    FILE *f;

    size = retro_serialize_size();
    if (size == 0)
    {
        CeLog("CeSaveState: retro_serialize_size returned 0");
        return 0;
    }

    buffer = malloc(size);
    if (!buffer)
    {
        CeLog("CeSaveState: malloc(%lu) failed", (unsigned long)size);
        return 0;
    }

    if (!retro_serialize(buffer, size))
    {
        free(buffer);
        CeLog("CeSaveState: retro_serialize failed");
        return 0;
    }

    _snwprintf(statePath, MAX_PATH + 8, L"%s.state", g_romPath);
    f = _wfopen(statePath, L"wb");
    if (!f)
    {
        free(buffer);
        CeLog("CeSaveState: failed to open state file for write");
        return 0;
    }

    fwrite(buffer, 1, size, f);
    fclose(f);
    free(buffer);
    CeLog("CeSaveState: saved %lu bytes", (unsigned long)size);
    return 1;
}

static int CeLoadState(HWND owner)
{
    wchar_t statePath[MAX_PATH + 8];
    FILE *f;
    long size;
    void *buffer;

    _snwprintf(statePath, MAX_PATH + 8, L"%s.state", g_romPath);
    f = _wfopen(statePath, L"rb");
    if (!f)
    {
        CeLog("CeLoadState: no state file found");
        CeShowMsgBox(owner, CeLangIsJapanese() ? L"\x30bb\x30fc\x30d6\x30c7\x30fc\x30bf\x304c\x898b\x3064\x304b\x308a\x307e\x305b\x3093\x3002" /* セーブデータが見つかりません。 */
                                                : L"No save state found.");
        return 0;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0)
    {
        fclose(f);
        CeLog("CeLoadState: empty/unreadable state file");
        CeShowMsgBox(owner, CeLangIsJapanese() ? L"\x30ed\x30fc\x30c9\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* ロードに失敗しました。 */
                                                : L"Load failed.");
        return 0;
    }

    buffer = malloc((size_t)size);
    if (!buffer)
    {
        fclose(f);
        CeLog("CeLoadState: malloc(%ld) failed", size);
        CeShowMsgBox(owner, CeLangIsJapanese() ? L"\x30ed\x30fc\x30c9\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* ロードに失敗しました。 */
                                                : L"Load failed.");
        return 0;
    }

    if (fread(buffer, 1, (size_t)size, f) != (size_t)size)
    {
        fclose(f);
        free(buffer);
        CeLog("CeLoadState: short read");
        CeShowMsgBox(owner, CeLangIsJapanese() ? L"\x30ed\x30fc\x30c9\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* ロードに失敗しました。 */
                                                : L"Load failed.");
        return 0;
    }
    fclose(f);

    if (!retro_unserialize(buffer, (size_t)size))
    {
        free(buffer);
        CeLog("CeLoadState: retro_unserialize failed");
        CeShowMsgBox(owner, CeLangIsJapanese() ? L"\x30ed\x30fc\x30c9\x306b\x5931\x6557\x3057\x307e\x3057\x305f(\x30bb\x30fc\x30d6\x30c7\x30fc\x30bf\x306e\x5f62\x5f0f\x304c\x7570\x306a\x308b\x53ef\x80fd\x6027\x304c\x3042\x308a\x307e\x3059)\x3002" /* ロードに失敗しました(セーブデータの形式が異なる可能性があります)。 */
                                                : L"Load failed (incompatible save?).");
        return 0;
    }

    free(buffer);
    CeLog("CeLoadState: loaded %ld bytes", size);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Menu (touch-to-reveal - hidden during gameplay, shown at startup     */
/* before any ROM is loaded and whenever the screen is tapped mid-game) */
/*                                                                      */
/* Implemented as a modal DialogBoxW (CE/ce_res.rc's IDD_MAINMENU) with */
/* plain PUSHBUTTON controls, not a real HMENU - see ce_resource.h for  */
/* why (this coredll doesn't export SetMenu).                          */
/* ------------------------------------------------------------------ */

static HINSTANCE g_hInstance = NULL;

/* ------------------------------------------------------------------ */
/* Generic message box (IDD_MSGBOX)                                   */
/*                                                                      */
/* MessageBoxW() draws with whatever system font Windows CE finds, and */
/* the Shinonome bitmap-font renderer (ce_bmpfont.h) can't reach that - */
/* a Japanese string passed to it renders as tofu boxes. Every result   */
/* message a user can see after Save/Load State is genuinely Japanese-  */
/* capable, so those go through this small owner-drawn dialog instead;  */
/* the handful of English-only startup/internal-error MessageBoxW calls */
/* (LoadRomFlow's "Failed to load ROM.", WinMain's RegisterClassW/       */
/* CreateWindowW failures) stay plain MessageBoxW() calls. Ported from  */
/* the sister QuickNES CE / gnuboy CE projects' own CeShowMsgBox()/      */
/* MsgBoxDlgProc(). */
/* ------------------------------------------------------------------ */

#define CE_MSGBOX_MAX_TEXT  256
static wchar_t s_msgBoxText[CE_MSGBOX_MAX_TEXT];

#define CE_MSGBOX_MAX_LINE  64
#define CE_MSGBOX_MAX_LINES 4

/* Word-wraps text into up to maxLines lines of at most maxWidth real
 * pixels each, filling lines[i][0..CE_MSGBOX_MAX_LINE-1]. Greedy: each
 * line takes as many characters as fit, breaking at the last ASCII space
 * on the line when there is one (so English breaks between words;
 * Japanese has no spaces and just hard-breaks). At least one character
 * always goes on a line even if it alone overflows. Any remainder past
 * the final line is dropped. Returns the number of lines used (>=1).
 * Ported from the sister PopSG/PicoDrive CE port (replaced this port's
 * earlier 2-line-only WrapMsgBoxText - the non-ASCII path message's
 * English text needs more than two lines). */
static int WrapMsgBoxLines(const wchar_t *text, int maxWidth,
                           wchar_t lines[][CE_MSGBOX_MAX_LINE], int maxLines)
{
    int count = 0;
    const wchar_t *p = text;

    while (*p && count < maxLines)
    {
        int fit = 0, lastSpace = -1, i;
        wchar_t probe[CE_MSGBOX_MAX_LINE];

        for (i = 0; p[i] && i < CE_MSGBOX_MAX_LINE - 1; i++)
        {
            probe[i] = p[i];
            probe[i + 1] = L'\0';
            if (CeBmpFontGetTextWidth(probe) > maxWidth)
                break;
            fit = i + 1;
            if (p[i] == L' ')
                lastSpace = i + 1;
        }

        if (p[fit] == L'\0')            /* rest fits on this line */
            ;
        else if (lastSpace > 0)         /* break after the last space */
            fit = lastSpace;
        else if (fit == 0)              /* one over-wide char - force it */
            fit = 1;

        wcsncpy(lines[count], p, fit);
        lines[count][fit] = L'\0';
        count++;

        p += fit;
        while (*p == L' ')              /* swallow the wrapped space */
            p++;
    }

    if (count == 0)
    {
        lines[0][0] = L'\0';
        count = 1;
    }
    return count;
}

/* Draws up to `count` pre-wrapped lines centered both ways in rc. */
static void PaintMsgBoxLines(HDC hdc, const RECT *rc,
                             wchar_t lines[][CE_MSGBOX_MAX_LINE], int count)
{
    COLORREF fg = GetSysColor(COLOR_WINDOWTEXT);
    int lineH  = CE_BMPFONT_HEIGHT + 2;
    int rectW  = rc->right - rc->left;
    int rectH  = rc->bottom - rc->top;
    int blockH = count * CE_BMPFONT_HEIGHT + (count - 1) * (lineH - CE_BMPFONT_HEIGHT);
    int y = rc->top + (rectH - blockH) / 2;
    int i;

    if (y < rc->top)   /* a block taller than rc grows downward, not up under the title bar */
        y = rc->top;

    SetBkMode(hdc, TRANSPARENT);
    for (i = 0; i < count; i++)
    {
        int x = rc->left + (rectW - CeBmpFontGetTextWidth(lines[i])) / 2;
        CeBmpFontDrawTextW(hdc, x, y, lines[i], fg);
        y += lineH;
    }
}

static WNDPROC s_pMsgBoxOkOrigProc = NULL;

/* Same DLGC_WANTALLKEYS/VK_RETURN/VK_SPACE/VK_ESCAPE subclass pattern as
 * every other BS_OWNERDRAW OK button in this port (MainMenuBtnCtrlProc
 * below, ce_video.c's VideoCtrlProc, ce_input.c's InputBtnCtrlProc) -
 * BS_OWNERDRAW breaks IsDialogMessage()'s normal DEFPUSHBUTTON Enter
 * routing and Escape-to-Cancel handling alike (real-hardware-confirmed
 * on the reference template), so both have to be
 * reimplemented by hand here too - an earlier comment claiming a lone
 * owner-draw OK button still got Enter for free was wrong. */
static LRESULT CALLBACK MsgBoxBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS;

    if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_RETURN:
        case VK_SPACE:
            SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)hWnd);
            return 0;

        case VK_ESCAPE:
            SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
            return 0;
        }
    }

    return CallWindowProc(s_pMsgBoxOkOrigProc, hWnd, message, wParam, lParam);
}

static INT_PTR CALLBACK MsgBoxDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        SetDlgItemTextW(hDlg, IDC_MB_TEXT, s_msgBoxText);
        ShowWindow(GetDlgItem(hDlg, IDC_MB_TEXT), SW_HIDE);
        s_pMsgBoxOkOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC, (LONG_PTR)MsgBoxBtnCtrlProc);
        return TRUE;

    case WM_DRAWITEM:
        CeBmpFontDrawOwnerButton((const DRAWITEMSTRUCT *)lParam);
        return TRUE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        wchar_t lines[CE_MSGBOX_MAX_LINES][CE_MSGBOX_MAX_LINE];
        int count;

        hdc = BeginPaint(hDlg, &ps);
        GetWindowRect(GetDlgItem(hDlg, IDC_MB_TEXT), &rc);
        MapWindowPoints(NULL, hDlg, (POINT *)&rc, 2);

        count = WrapMsgBoxLines(s_msgBoxText, rc.right - rc.left, lines, CE_MSGBOX_MAX_LINES);
        PaintMsgBoxLines(hdc, &rc, lines, count);

        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
}

static void CeShowMsgBox(HWND owner, const wchar_t *text)
{
    wcsncpy(s_msgBoxText, text, CE_MSGBOX_MAX_TEXT - 1);
    s_msgBoxText[CE_MSGBOX_MAX_TEXT - 1] = L'\0';
    DialogBoxW(g_hInstance, MAKEINTRESOURCEW(IDD_MSGBOX), owner, MsgBoxDlgProc);
}

/* ------------------------------------------------------------------ */
/* Yes/No confirmation (IDD_CONFIRM) - user request: Save State asks     */
/* first. Copied verbatim from the gnuboy CE port's ConfirmDlgProc/     */
/* ConfirmBtnCtrlProc/CeConfirm (DialogBoxW takes g_hInstance, to match  */
/* this port's own MsgBoxDlgProc). Shares WrapMsgBoxLines()/            */
/* PaintMsgBoxLines()/s_msgBoxText with CeShowMsgBox above (never on     */
/* screen at the same time), and the same BS_OWNERDRAW-button key        */
/* handling; just two buttons instead of one. */
/* ------------------------------------------------------------------ */

static WNDPROC s_pConfirmBtnOrigProc = NULL;
static int     s_ssPhase             = 0; /* 0 = asking, 1 = showing result */

static LRESULT CALLBACK ConfirmBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    int id = GetDlgCtrlID(hWnd);
    HWND hDlg = GetParent(hWnd);

    if (message == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS;

    if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_RETURN:
        case VK_SPACE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)hWnd);
            return 0;

        case VK_ESCAPE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDC_CF_NO, 0), (LPARAM)hWnd);
            return 0;

        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
            if (s_ssPhase == 0) /* the result phase has only the OK button */
                SetFocus(GetDlgItem(hDlg, id == IDC_CF_YES ? IDC_CF_NO : IDC_CF_YES));
            return 0;
        }
    }

    return CallWindowProc(s_pConfirmBtnOrigProc, hWnd, message, wParam, lParam);
}

/* Save State dialog: asks, runs the save in place on Yes, then swaps its
 * own text/buttons to the result acknowledgement - never opening a
 * second (nested) modal, which is what made the main menu's
 * "ステートセーブ" button flash to the front during the old
 * confirm -> message-box hand-off. */
static INT_PTR CALLBACK SaveStateDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        s_ssPhase = 0;
        ShowWindow(GetDlgItem(hDlg, IDC_CF_TEXT), SW_HIDE);
        SetDlgItemTextW(hDlg, IDC_CF_YES, CeLangIsJapanese() ? L"\x306f\x3044"       /* はい */ : L"Yes");
        SetDlgItemTextW(hDlg, IDC_CF_NO,  CeLangIsJapanese() ? L"\x3044\x3044\x3048" /* いいえ */ : L"No");
        s_pConfirmBtnOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDC_CF_YES), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_CF_YES), GWLP_WNDPROC, (LONG_PTR)ConfirmBtnCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_CF_NO),  GWLP_WNDPROC, (LONG_PTR)ConfirmBtnCtrlProc);
        SetActiveWindow(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_CF_YES));
        return FALSE;

    case WM_DRAWITEM:
        CeBmpFontDrawOwnerButton((const DRAWITEMSTRUCT *)lParam);
        return TRUE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        wchar_t lines[CE_MSGBOX_MAX_LINES][CE_MSGBOX_MAX_LINE];
        int count;

        hdc = BeginPaint(hDlg, &ps);
        GetWindowRect(GetDlgItem(hDlg, IDC_CF_TEXT), &rc);
        MapWindowPoints(NULL, hDlg, (POINT *)&rc, 2);

        count = WrapMsgBoxLines(s_msgBoxText, rc.right - rc.left, lines, CE_MSGBOX_MAX_LINES);
        PaintMsgBoxLines(hdc, &rc, lines, count);

        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_CF_YES:
            if (s_ssPhase == 0)
            {
                int ok = CeSaveState();
                wcsncpy(s_msgBoxText, CeLangIsJapanese()
                        ? (ok ? L"\x30bb\x30fc\x30d6\x3057\x307e\x3057\x305f\x3002"                     /* セーブしました。 */
                              : L"\x30bb\x30fc\x30d6\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002")  /* セーブに失敗しました。 */
                        : (ok ? L"State saved." : L"Save failed."),
                        CE_MSGBOX_MAX_TEXT - 1);
                s_msgBoxText[CE_MSGBOX_MAX_TEXT - 1] = L'\0';
                s_ssPhase = 1;
                ShowWindow(GetDlgItem(hDlg, IDC_CF_NO), SW_HIDE);
                SetDlgItemTextW(hDlg, IDC_CF_YES, L"OK");
                {   /* recentre the lone OK button */
                    RECT rc, rb;
                    GetClientRect(hDlg, &rc);
                    GetWindowRect(GetDlgItem(hDlg, IDC_CF_YES), &rb);
                    MapWindowPoints(NULL, hDlg, (POINT *)&rb, 2);
                    SetWindowPos(GetDlgItem(hDlg, IDC_CF_YES), NULL,
                                 (rc.right - (rb.right - rb.left)) / 2, rb.top,
                                 0, 0, SWP_NOSIZE | SWP_NOZORDER);
                }
                InvalidateRect(hDlg, NULL, TRUE);
                SetFocus(GetDlgItem(hDlg, IDC_CF_YES));
            }
            else
            {
                EndDialog(hDlg, 1);
            }
            return TRUE;
        case IDC_CF_NO:
        case IDCANCEL:
            EndDialog(hDlg, s_ssPhase ? 1 : 0);
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
}

/* Save State: asks, runs the save, acknowledges the result - all in one
 * self-drawn dialog (never a nested second modal). */
static void CeConfirmAndSaveState(HWND owner)
{
    wcsncpy(s_msgBoxText, CeLangIsJapanese()
            ? L"\x30bb\x30fc\x30d6\x3057\x307e\x3059\x304b\xff1f" /* セーブしますか？ */
            : L"Save state?",
            CE_MSGBOX_MAX_TEXT - 1);
    s_msgBoxText[CE_MSGBOX_MAX_TEXT - 1] = L'\0';

    DialogBoxW(g_hInstance, MAKEINTRESOURCEW(IDD_CONFIRM), owner, SaveStateDlgProc);
}

/* ------------------------------------------------------------------ */
/* Screenshot                                                          */
/* ------------------------------------------------------------------ */

static void PutLE16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void PutLE32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* Saves the paused game image as a 16-bit RGB565 BMP (BI_BITFIELDS -
 * the native pixel format of both sources below, so no conversion and no
 * compression: one fwrite per row). Uses the on-screen image at the
 * current Scale (x1/x1.5/Wide/Full, from ce_display.c's DIB) so the file
 * matches what the user sees; falls back to the core's unscaled frame
 * if the display has nothing. Written to "<exe-dir>\Screenshots\<ROM name>_NNN.bmp",
 * creating the folder on first use and taking the first unused number.
 * The header is built byte by byte rather than from BITMAPFILEHEADER so
 * struct packing can't shift it. Returns 1 on success. */
static int CeSaveScreenshot(void)
{
    wchar_t dir[MAX_PATH];
    wchar_t romName[MAX_PATH];
    wchar_t path[MAX_PATH + 16];
    wchar_t *p;
    const wchar_t *base;
    unsigned n, y, rowBytes, padBytes;
    unsigned long imageBytes;
    unsigned char hdr[66];
    static const unsigned char pad[4] = { 0, 0, 0, 0 };
    const void *img;
    unsigned imgW, imgH, imgPitch;
    FILE *f;

    /* g_lastFrame doubles as "something was drawn since this ROM
     * loaded" - the display's DIB isn't cleared on ROM switch, so
     * without it a new game could save the previous game's image. */
    if (!g_lastFrame || g_lastFrameW == 0 || g_lastFrameH == 0)
    {
        CeLog("CeSaveScreenshot: no rendered frame yet");
        return 0;
    }
    if (!CeDisplayGetLastImage(&img, &imgW, &imgH, &imgPitch))
    {
        img = g_lastFrame;
        imgW = g_lastFrameW;
        imgH = g_lastFrameH;
        imgPitch = (unsigned)g_lastFramePitch;
    }

    if (!GetModuleFileNameW(NULL, dir, MAX_PATH))
        return 0;
    p = wcsrchr(dir, L'\\');
    if (!p)
        return 0;
    p[1] = L'\0';
    if (wcslen(dir) + 12 >= MAX_PATH)
        return 0;
    wcscat(dir, L"Screenshots");
    CreateDirectoryW(dir, NULL); /* already existing is fine */
    if (GetFileAttributesW(dir) == 0xFFFFFFFF)
    {
        CeLog("CeSaveScreenshot: can't create Screenshots folder");
        return 0;
    }

    base = wcsrchr(g_romPath, L'\\');
    wcsncpy(romName, base ? base + 1 : g_romPath, MAX_PATH - 1);
    romName[MAX_PATH - 1] = L'\0';
    p = wcsrchr(romName, L'.');
    if (p)
        *p = L'\0';

    for (n = 1; n <= 999; n++)
    {
        _snwprintf(path, MAX_PATH + 16, L"%s\\%s_%03u.bmp", dir, romName, n);
        path[MAX_PATH + 15] = L'\0';
        if (GetFileAttributesW(path) == 0xFFFFFFFF)
            break;
    }
    if (n > 999)
    {
        CeLog("CeSaveScreenshot: all 999 numbers used");
        return 0;
    }

    rowBytes = imgW * 2;
    padBytes = (4 - (rowBytes & 3)) & 3;
    imageBytes = (unsigned long)(rowBytes + padBytes) * imgH;

    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    PutLE32(hdr + 2, sizeof(hdr) + imageBytes);  /* file size */
    PutLE32(hdr + 10, sizeof(hdr));              /* pixel data offset */
    PutLE32(hdr + 14, 40);                       /* BITMAPINFOHEADER size */
    PutLE32(hdr + 18, imgW);
    PutLE32(hdr + 22, imgH);             /* positive = bottom-up */
    PutLE16(hdr + 26, 1);                        /* planes */
    PutLE16(hdr + 28, 16);                       /* bits per pixel */
    PutLE32(hdr + 30, 3);                        /* BI_BITFIELDS */
    PutLE32(hdr + 34, imageBytes);
    PutLE32(hdr + 38, 2835);                     /* 72 dpi */
    PutLE32(hdr + 42, 2835);
    PutLE32(hdr + 54, 0xF800);                   /* R mask */
    PutLE32(hdr + 58, 0x07E0);                   /* G mask */
    PutLE32(hdr + 62, 0x001F);                   /* B mask */

    f = _wfopen(path, L"wb");
    if (!f)
    {
        CeLog("CeSaveScreenshot: can't open output file");
        return 0;
    }
    fwrite(hdr, 1, sizeof(hdr), f);
    for (y = imgH; y-- > 0; )
    {
        fwrite((const unsigned char *)img + (size_t)y * imgPitch, 1, rowBytes, f);
        if (padBytes)
            fwrite(pad, 1, padBytes, f);
    }
    if (ferror(f))
    {
        fclose(f);
        DeleteFileW(path);
        CeLog("CeSaveScreenshot: write failed");
        return 0;
    }
    fclose(f);

    CeLog("CeSaveScreenshot: saved %ux%u as #%03u", imgW, imgH, n);
    return 1;
}

static void CeScreenshotAndReport(HWND owner)
{
    if (CeSaveScreenshot())
        CeShowMsgBox(owner, CeLangIsJapanese()
            ? L"\x30b9\x30af\x30ea\x30fc\x30f3\x30b7\x30e7\x30c3\x30c8\x3092\x4fdd\x5b58\x3057\x307e\x3057\x305f\x3002" /* スクリーンショットを保存しました。 */
            : L"Screenshot saved.");
    else
        CeShowMsgBox(owner, CeLangIsJapanese()
            ? L"\x30b9\x30af\x30ea\x30fc\x30f3\x30b7\x30e7\x30c3\x30c8\x306e\x4fdd\x5b58\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* スクリーンショットの保存に失敗しました。 */
            : L"Screenshot failed.");
}

/* Swaps every IDD_MAINMENU button caption between English (the .rc
 * template's own text) and Japanese, gated by CeLangIsJapanese() - see
 * ce_lang.h. Called once from WM_INITDIALOG, and again right after the
 * IDC_MM_VIDEO case returns (Video Config is where the toggle itself
 * lives), so flipping it and returning to this still-open dialog updates
 * it immediately instead of only the next time the menu happens to be
 * recreated. The title bar ("PopSNES") is deliberately never touched -
 * it's non-client area the OS paints with its own caption font, and was
 * never reachable by WM_SETFONT anyway. Buttons draw their own text via
 * WM_DRAWITEM (CeBmpFontDrawOwnerButton(), ce_bmpfont.h) now, so this
 * function only needs to set text and hide/show IDC_MM_HINT (the one
 * plain LTEXT here) for WM_PAINT to repaint via CeBmpFontPaintLabel(). */
static void ApplyMainMenuLanguage(HWND hDlg)
{
    if (CeLangIsJapanese())
    {
        SetDlgItemTextW(hDlg, IDC_MM_OPEN,      L"ROM\x3092\x958b\x304f...");                             /* ROMを開く... */
        SetDlgItemTextW(hDlg, IDC_MM_SAVESTATE, L"\x30b9\x30c6\x30fc\x30c8\x30bb\x30fc\x30d6");            /* ステートセーブ */
        SetDlgItemTextW(hDlg, IDC_MM_LOADSTATE, L"\x30b9\x30c6\x30fc\x30c8\x30ed\x30fc\x30c9");            /* ステートロード */
        SetDlgItemTextW(hDlg, IDC_MM_INPUT,     L"\x30dc\x30bf\x30f3\x8a2d\x5b9a");                        /* ボタン設定 */
        SetDlgItemTextW(hDlg, IDC_MM_SOUND,     L"\x30b5\x30a6\x30f3\x30c9\x8a2d\x5b9a");                  /* サウンド設定 */
        SetDlgItemTextW(hDlg, IDC_MM_VIDEO,     L"\x753b\x9762\x8a2d\x5b9a");                              /* 画面設定 */
        SetDlgItemTextW(hDlg, IDC_MM_SCREENSHOT, L"\x753b\x9762\x4fdd\x5b58\x3059\x308b"); /* 画面保存する */
        SetDlgItemTextW(hDlg, IDC_MM_EXIT,      L"\x7d42\x4e86");                                          /* 終了 */
        SetDlgItemTextW(hDlg, IDC_MM_HINT,      L"\x623b\x308b\x30ad\x30fc\x3067\x30b2\x30fc\x30e0\x518d\x958b"); /* 戻るキーでゲーム再開 */
    }
    else
    {
        SetDlgItemTextW(hDlg, IDC_MM_OPEN,      L"Open ROM...");
        SetDlgItemTextW(hDlg, IDC_MM_SAVESTATE, L"Save State");
        SetDlgItemTextW(hDlg, IDC_MM_LOADSTATE, L"Load State");
        SetDlgItemTextW(hDlg, IDC_MM_INPUT,     L"Input Cfg");
        SetDlgItemTextW(hDlg, IDC_MM_SOUND,     L"Sound Cfg");
        SetDlgItemTextW(hDlg, IDC_MM_VIDEO,     L"Video Cfg");
        SetDlgItemTextW(hDlg, IDC_MM_SCREENSHOT, L"Screenshot");
        SetDlgItemTextW(hDlg, IDC_MM_EXIT,      L"Exit");
        SetDlgItemTextW(hDlg, IDC_MM_HINT,      L"Press Back to resume the game.");
    }

    ShowWindow(GetDlgItem(hDlg, IDC_MM_HINT), SW_HIDE);

    /* CeBmpFontPaintLabel()'s SetPixel() drawing is transparent (never
     * clears the rect first), so switching languages on an already-open
     * dialog needs a forced full repaint or the old caption's glyphs
     * would still show through the new ones. */
    InvalidateRect(hDlg, NULL, TRUE);
}

static const int kMainMenuButtonIds[] = {
    IDC_MM_OPEN, IDC_MM_SAVESTATE, IDC_MM_LOADSTATE,
    IDC_MM_VIDEO, IDC_MM_SOUND, IDC_MM_INPUT, IDC_MM_SCREENSHOT, IDC_MM_EXIT,
};
#define CE_MAINMENU_BUTTON_COUNT (sizeof(kMainMenuButtonIds) / sizeof(kMainMenuButtonIds[0]))

/* Skips disabled buttons (Save/Load State while g_romLoaded is still 0) -
 * EnableWindow() alone only blocks activation, not this dialog's own
 * custom arrow-key cycling below. Bounded to CE_MAINMENU_BUTTON_COUNT
 * steps so it can't spin forever. Ported from the reference
 * template's own MainMenuNeighbor(). */
static int MainMenuNeighbor(HWND hDlg, int id, int delta)
{
    int idx, step;
    for (idx = 0; idx < (int)CE_MAINMENU_BUTTON_COUNT; idx++)
        if (kMainMenuButtonIds[idx] == id)
            break;
    if (idx >= (int)CE_MAINMENU_BUTTON_COUNT)
        return id;

    for (step = 1; step <= (int)CE_MAINMENU_BUTTON_COUNT; step++)
    {
        int nextIdx = ((idx + delta * step) % (int)CE_MAINMENU_BUTTON_COUNT + (int)CE_MAINMENU_BUTTON_COUNT) % (int)CE_MAINMENU_BUTTON_COUNT;
        int nextId = kMainMenuButtonIds[nextIdx];
        if (IsWindowEnabled(GetDlgItem(hDlg, nextId)))
            return nextId;
    }
    return id;
}

/* Colorful rounded-button skin, ported from the sister PopSG port's main
 * menu (user request): pastel fills with a darker same-hue outline drawn
 * by CeBmpFontDrawOwnerButtonTheme() (ce_bmpfont.c), a cream client
 * background (WM_ERASEBKGND below) and the mascot bitmap in the footer.
 * Only this dialog uses it - every other dialog keeps the plain gray
 * CeBmpFontDrawOwnerButton() look. Colors match PopSG's table; the
 * Screenshot button (PopSNES only) gets its own lavender pair. */
#define CE_MENU_BG_CREAM   RGB(0xF0, 0xE1, 0xBC)
#define CE_MENU_TEXT_DARK  RGB(0x2A, 0x2C, 0x30)

typedef struct { int id; COLORREF bg, border; CeMenuIcon icon; int stacked; } CeMenuButtonTheme;

static const CeMenuButtonTheme kMainMenuTheme[] = {
    { IDC_MM_OPEN,       RGB(0x6F, 0xA8, 0xDC), RGB(0x1D, 0x4A, 0x70), CE_MENU_ICON_OPEN,       0 }, /* blue         */
    { IDC_MM_SAVESTATE,  RGB(0xF5, 0xEC, 0x9E), RGB(0x6E, 0x66, 0x12), CE_MENU_ICON_SAVE,       0 }, /* pastel lemon */
    { IDC_MM_LOADSTATE,  RGB(0xBF, 0xE3, 0xD0), RGB(0x1D, 0x5A, 0x3C), CE_MENU_ICON_LOAD,       0 }, /* mint         */
    { IDC_MM_VIDEO,      RGB(0xC9, 0xE4, 0xB0), RGB(0x3C, 0x5A, 0x1B), CE_MENU_ICON_VIDEO,      1 }, /* green        */
    { IDC_MM_SOUND,      RGB(0xB9, 0xD7, 0xEE), RGB(0x1D, 0x4A, 0x70), CE_MENU_ICON_SOUND,      1 }, /* blue         */
    { IDC_MM_INPUT,      RGB(0xF2, 0xB8, 0xC6), RGB(0x7A, 0x2E, 0x4C), CE_MENU_ICON_INPUT,      1 }, /* pink         */
    { IDC_MM_SCREENSHOT, RGB(0xD9, 0xCC, 0xF0), RGB(0x4E, 0x34, 0x80), CE_MENU_ICON_SCREENSHOT, 0 }, /* lavender     */
    { IDC_MM_EXIT,       RGB(0xF0, 0xA9, 0xA0), RGB(0x7A, 0x23, 0x18), CE_MENU_ICON_EXIT,       0 }, /* coral        */
};

/* CE/popsnes_mascot.bmp (IDB_MAINMENU, ce_res.rc). Loaded in
 * WM_INITDIALOG, blitted by WM_PAINT into the blank strip right of the
 * IDC_MM_HINT text, freed in WM_DESTROY. */
static HBITMAP s_hMainMenuBmp = NULL;

static WNDPROC s_pMainMenuOrigProc = NULL;

/* PUSHBUTTON's built-in WM_KEYDOWN -> BN_CLICKED conversion for
 * VK_RETURN/VK_SPACE (and IsDialogMessage()'s own Up/Down/Left/Right
 * focus-cycling) stops working once these buttons become BS_OWNERDRAW
 * (ce_res.rc) - real-hardware-confirmed by the reference
 * template: touch/stylus taps kept working (WM_LBUTTONUP is unaffected)
 * but the physical decide key did nothing on a focused button. Fixed
 * here the same way every other owner-draw button in this port already
 * is (ce_video.c's VideoCtrlProc, ce_input.c's InputBtnCtrlProc): claim
 * DLGC_WANTARROWS | DLGC_WANTALLKEYS and reimplement arrow navigation
 * via MainMenuNeighbor() plus the decide/Back keys by hand. */
static LRESULT CALLBACK MainMenuBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    int id = GetDlgCtrlID(hWnd);
    HWND hDlg = GetParent(hWnd);

    if (message == WM_GETDLGCODE)
        return DLGC_WANTARROWS | DLGC_WANTALLKEYS;

    if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_UP:
        case VK_LEFT:
            SetFocus(GetDlgItem(hDlg, MainMenuNeighbor(hDlg, id, -1)));
            return 0;

        case VK_DOWN:
        case VK_RIGHT:
            SetFocus(GetDlgItem(hDlg, MainMenuNeighbor(hDlg, id, 1)));
            return 0;

        case VK_RETURN:
        case VK_SPACE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)hWnd);
            return 0;

        case VK_ESCAPE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
            return 0;
        }
    }

    return CallWindowProc(s_pMainMenuOrigProc, hWnd, message, wParam, lParam);
}

static INT_PTR CALLBACK MainMenuDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        unsigned i;
        EnableWindow(GetDlgItem(hDlg, IDC_MM_SAVESTATE), g_romLoaded);
        EnableWindow(GetDlgItem(hDlg, IDC_MM_LOADSTATE), g_romLoaded);
        EnableWindow(GetDlgItem(hDlg, IDC_MM_SCREENSHOT), g_romLoaded);
        ApplyMainMenuLanguage(hDlg);

        s_pMainMenuOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDC_MM_OPEN), GWLP_WNDPROC);
        for (i = 0; i < CE_MAINMENU_BUTTON_COUNT; i++)
            SetWindowLongPtrW(GetDlgItem(hDlg, kMainMenuButtonIds[i]), GWLP_WNDPROC, (LONG_PTR)MainMenuBtnCtrlProc);

        if (!s_hMainMenuBmp)
            s_hMainMenuBmp = LoadBitmapW(g_hInstance, MAKEINTRESOURCEW(IDB_MAINMENU));
        return TRUE;
    }

    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
        unsigned i;
        for (i = 0; i < sizeof(kMainMenuTheme) / sizeof(kMainMenuTheme[0]); i++)
        {
            if (kMainMenuTheme[i].id == (int)dis->CtlID)
            {
                CeBmpFontDrawOwnerButtonTheme(dis, kMainMenuTheme[i].bg, kMainMenuTheme[i].border, CE_MENU_TEXT_DARK,
                                              kMainMenuTheme[i].icon, kMainMenuTheme[i].stacked, CE_MENU_BG_CREAM);
                return TRUE;
            }
        }
        CeBmpFontDrawOwnerButton(dis); /* fallback, no control here should hit it */
        return TRUE;
    }

    /* Cream client background, scoped to just this dialog. The brush is
     * created once and kept for the process lifetime (this fires on
     * every erase, e.g. each time a sub-dialog closes over this one). */
    case WM_ERASEBKGND:
    {
        static HBRUSH s_hCreamBrush = NULL;
        RECT rc;
        if (!s_hCreamBrush)
            s_hCreamBrush = CreateSolidBrush(CE_MENU_BG_CREAM);
        GetClientRect(hDlg, &rc);
        FillRect((HDC)wParam, &rc, s_hCreamBrush);
        return TRUE;
    }

    case WM_DESTROY:
        if (s_hMainMenuBmp)
        {
            DeleteObject(s_hMainMenuBmp);
            s_hMainMenuBmp = NULL;
        }
        return FALSE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hDlg, &ps);
        CeBmpFontPaintLabel(hdc, hDlg, IDC_MM_HINT);

        /* Mascot in the blank strip right of the hint text: starts just
         * past the text, runs to the dialog's right/bottom edge, source
         * aspect ratio preserved, bottom-right aligned (same as PopSG). */
        if (s_hMainMenuBmp)
        {
            HWND hHint = GetDlgItem(hDlg, IDC_MM_HINT);
            RECT rcHint, rcClient;
            wchar_t hintText[128];
            BITMAP bm;

            hintText[0] = 0;
            GetWindowTextW(hHint, hintText, 128);
            GetWindowRect(hHint, &rcHint);
            MapWindowPoints(NULL, hDlg, (POINT *)&rcHint, 2);
            GetClientRect(hDlg, &rcClient);

            if (GetObject(s_hMainMenuBmp, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0)
            {
                long boxL = rcHint.left + CeBmpFontGetTextWidth(hintText) + 8;
                long boxR = rcClient.right - 4;
                long boxT = rcHint.top;
                long boxB = rcClient.bottom - 2;
                long boxW = boxR - boxL;
                long boxH = boxB - boxT;

                if (boxW > 8 && boxH > 8)
                {
                    long drawW = boxW;
                    long drawH = drawW * bm.bmHeight / bm.bmWidth;
                    HDC memDC;
                    HGDIOBJ oldBmp;

                    if (drawH > boxH)
                    {
                        drawH = boxH;
                        drawW = drawH * bm.bmWidth / bm.bmHeight;
                    }

                    /* No SetStretchBltMode() - this coredll doesn't export
                     * it; CE's default (COLORONCOLOR) is fine here. */
                    memDC = CreateCompatibleDC(hdc);
                    oldBmp = SelectObject(memDC, s_hMainMenuBmp);
                    StretchBlt(hdc, (int)(boxR - drawW), (int)(boxB - drawH),
                               (int)drawW, (int)drawH,
                               memDC, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                    SelectObject(memDC, oldBmp);
                    DeleteDC(memDC);
                }
            }
        }

        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_MM_OPEN:
            /* Stays open on cancel/failure (LoadRomFlow already showed
             * a MessageBox explaining why); closes only on success, so
             * the caller knows to resume gameplay. */
            if (LoadRomFlow(hDlg))
                EndDialog(hDlg, IDC_MM_OPEN);
            return TRUE;

        case IDC_MM_EXIT:
            CeShutdown(0); /* never returns */
            return TRUE;

        case IDC_MM_SAVESTATE:
            if (g_romLoaded)
                CeConfirmAndSaveState(hDlg);
            return TRUE; /* stays open either way, like Input/Sound Config */

        case IDC_MM_LOADSTATE:
            /* Closes and resumes on success (like Resume Game) so the
             * user immediately sees the loaded state; stays open on
             * failure (CeLoadState already showed why). */
            if (g_romLoaded && CeLoadState(hDlg))
                EndDialog(hDlg, IDC_MM_LOADSTATE);
            return TRUE;

        case IDC_MM_SCREENSHOT:
            if (g_romLoaded)
                CeScreenshotAndReport(hDlg);
            return TRUE; /* stays open, like Save State */

        case IDC_MM_INPUT:
            CeShowInputConfigDialog(hDlg);
            return TRUE;

        case IDC_MM_SOUND:
            CeShowSoundConfigDialog(hDlg);
            return TRUE;

        case IDC_MM_VIDEO:
            CeShowVideoConfigDialog(hDlg);
            /* Video Config is where the Japanese/English toggle lives -
             * re-apply here so switching it and returning to this
             * still-open menu updates it immediately. */
            ApplyMainMenuLanguage(hDlg);
            return TRUE;

        case IDCANCEL:
            /* Hardware Back / OS close gesture: same as Resume if a
             * game is already running (nothing to lose by dismissing),
             * otherwise ignored - there's nothing to go back to yet. */
            if (g_romLoaded)
                EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
}

/* Pauses (if a game is running), shows the menu dialog modally (blocks
 * until closed), then resumes if a game is loaded when it returns -
 * whether that's the game that was already running, or one just picked
 * via Open ROM. Also how the very first "no ROM loaded" screen is shown
 * from WinMain, where wasPlaying is simply false. */
static void ShowMainMenuDialog(HWND hwnd)
{
    int wasPlaying = g_romLoaded && !g_paused;

    if (wasPlaying)
    {
        g_paused = 1;
        CeAudioSetPaused(1); /* the ring starves every buffer while the menu is up - tell the drain thread that's expected, not an underrun */
        CeDisplaySuspend(); /* releases the cached window DC; no full teardown - see ce_display.h */

        /* Autosave SRAM at every pause, not just at graceful shutdown/
         * ROM switch - a real power-off doesn't run CeShutdown() at all
         * (round 9 user report: an in-cartridge save made this session
         * wasn't on disk after the device was actually powered off, not
         * just app-restarted), so relying only on those two checkpoints
         * misses that case entirely. Opening the touch-to-reveal menu is
         * a frequent, cheap, natural checkpoint to also save at. */
        CeSaveSram();
    }

    InvalidateRect(hwnd, NULL, TRUE);
    DialogBoxW(g_hInstance, MAKEINTRESOURCEW(IDD_MAINMENU), hwnd, MainMenuDlgProc);

    if (g_romLoaded)
    {
        g_paused = 0;
        g_paceResync = 1; /* menu time must not count as "behind" */
        CeAudioSetPaused(0);
        if (!CeDisplayInit(hwnd))
            CeLog("ShowMainMenuDialog: CeDisplayInit failed - continuing without video output");
        CeDisplayResume(); /* no-op under GDI - see ce_display.h */

        /* Re-assert taskbar hiding every time gameplay is (re-)entered,
         * not just once at WinMain startup - user-reported the taskbar
         * still wasn't hidden (2026-08-01) even after two earlier
         * attempts (WS_POPUP + reassert ShowWindow/UpdateWindow, then
         * keeping aygshell.dll loaded for the app's lifetime) that both
         * matched confirmed-working reference samples exactly. The one
         * structural difference this app has that neither reference
         * sample does: a custom DialogBoxW (IDD_MAINMENU) becomes the
         * foreground window immediately after the original SHFullScreen
         * call, every time the touch-to-reveal menu opens - if this
         * device's shell restores the taskbar whenever some other
         * top-level window takes focus (plausible, unconfirmed), the
         * single startup-time call would never be enough. Calling it
         * again right here, every time control returns from the menu
         * dialog to gameplay, should be robust to that regardless of
         * the exact underlying reason. */
        CeHideShellChrome(hwnd);
    }
}

/* ------------------------------------------------------------------ */
/* Window / shutdown                                                   */
/* ------------------------------------------------------------------ */

/* Same filter as ce_input.c's IsRoutineDialogChatter() (see that
 * function's comment) - kept as its own local copy rather than a shared
 * export since this is a small, self-contained diagnostic filter, not
 * real cross-module functionality. */
static int IsRoutineWindowChatter(UINT msg)
{
    switch (msg)
    {
    case WM_PAINT:          case WM_NCPAINT:
    case WM_ERASEBKGND:     case WM_SETCURSOR:
    case WM_NCHITTEST:      case WM_MOUSEMOVE:
    case WM_NCMOUSEMOVE:    case WM_GETTEXT:
    case WM_GETTEXTLENGTH:
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:    case WM_CTLCOLORSCROLLBAR:
    case WM_CTLCOLORSTATIC:
        return 1;
    default:
        return 0;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DESTROY:
        g_running = 0;
        PostQuitMessage(0);
        return 0;

    case WM_LBUTTONDOWN:
        /* Touch-to-reveal, same interaction the previous CE port used.
         * ShowMainMenuDialog() is modal, so this can't re-enter while
         * already showing (input goes to the dialog, not this window). */
        ShowMainMenuDialog(hwnd);
        return 0;

    case WM_PAINT:
    {
        /* GDI video output (ce_display.c) draws straight into this
         * window's own client area, so - unlike an earlier GAPI backend,
         * which bypassed window painting entirely while it owned the
         * display - a WM_PAINT here (a modal dialog covering part of the
         * game window, then closing) needs the last rendered frame
         * redrawn, or the exposed region stays black until the next real
         * emulated frame (which won't happen at all while retro_run() is
         * paused for that same dialog). Erase the invalidated region to
         * black first (covers both the "No ROM loaded" screen and any
         * letterbox border around the game rect), then, if a ROM is
         * loaded, redraw the current frame on top via
         * CeDisplayForceRepaint(). */
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        FillRect(hdc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(hwnd, &ps);
        if (g_romLoaded)
            CeDisplayForceRepaint();
        return 0;
    }

    default:
        /* Diagnostic for the still-undetected "\x6c7a\x5b9a" (Decide/OK)
         * hardware button (round 7, 2026-08-01): InputConfigDlgProc's
         * own diagnostic logging (ce_input.c) saw nothing at all when it
         * was pressed, even though that dialog is modal and should own
         * the message queue - one remaining possibility is that this
         * button's message (if it sends one at all) targets the *main*
         * frame window (this WndProc) rather than the dialog, since a
         * modal dialog's message loop still dispatches by target HWND,
         * not focus. Logs here too while a remap is pending, so the
         * next hardware round can rule this in or out - filtered through
         * IsRoutineWindowChatter() (round 8) after round 7's unfiltered
         * version in ce_input.c logged ordinary button-repaint chatter
         * (WM_CTLCOLORBTN) and it looked like a candidate when it wasn't;
         * this window gets its own share of paint/cursor housekeeping
         * too, so the same filter applies here. */
        if (CeInputIsWaitingForKeyRemap() && !IsRoutineWindowChatter(msg))
            CeLog("WndProc: msg=0x%04X wParam=0x%08X while an Input Config remap is pending",
                  (unsigned)msg, (unsigned)wParam);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static void CeShutdown(int exitCode)
{
    /* Lesson from the previous CE port: a plain
     * `return` from WinMain lets the C++ runtime's normal exit path run,
     * which on this device/toolchain combination could itself crash or
     * hang threads mid-teardown. Always terminate via ExitProcess. This
     * matters more now than when this comment was first written: the
     * audio thread (CE/ce_audio.c) is a real background thread, and
     * CeAudioStop() below joins it cleanly before we ever get here, so
     * ExitProcess() isn't tearing down a thread still mid-waveOutWrite. */
    CeAudioStop();
    if (g_romLoaded)
        CeSaveSram();
    retro_unload_game();
    retro_deinit();

    CeDisplayShutdown();
    CeShowShellChrome(g_hwnd);

    if (g_romData)
    {
        free(g_romData);
        g_romData = NULL;
    }

    if (g_mutex)
        CloseHandle(g_mutex);

    ExitProcess((UINT)exitCode);
}

/* "HHTaskBar" is the standard window class of the Windows CE Explorer
 * taskbar (the desktop-style one visible in the PW-G5200's own shell,
 * as opposed to a Pocket PC command bar) - FindWindow+ShowWindow(HIDE)
 * on it is the documented fallback for CE builds where SHFullScreen
 * doesn't apply. This device's shell is the fuller desktop-like CE
 * Explorer seen in the reference screenshot, taskbar and all, not the
 * Pocket PC chrome SHFullScreen targets, so this direct approach is used
 * exclusively now - aygshell.dll (which only ever provided SHFullScreen
 * here) has been dropped entirely: this device's aygshell.dll is a
 * bundled, device-specific file, not a component every
 * Windows CE target is guaranteed to ship, so depending on it made the
 * build fragile on other devices/images. The FindWindow+ShowWindow
 * approach below needs nothing beyond coredll.dll and works whether or
 * not aygshell.dll is even present. */
static HWND CeFindTaskBarWindow(void)
{
    return FindWindowW(L"HHTaskBar", NULL);
}

static void CeHideShellChrome(HWND hwnd)
{
    HWND hTaskBar = CeFindTaskBarWindow();
    (void)hwnd;
    if (hTaskBar)
    {
        ShowWindow(hTaskBar, SW_HIDE);
        CeLog("CeHideShellChrome: hid HHTaskBar window directly");
    }
    else
    {
        CeLog("CeHideShellChrome: HHTaskBar window not found");
    }
}

/* Restores shell chrome on exit - this is a shared-shell CE device, not
 * a single-purpose game handheld, so leaving the taskbar hidden after
 * this app closes would affect the user's other apps until reboot. */
static void CeShowShellChrome(HWND hwnd)
{
    HWND hTaskBar = CeFindTaskBarWindow();
    (void)hwnd;
    if (hTaskBar)
        ShowWindow(hTaskBar, SW_SHOW);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSW wc;
    MSG msg;

    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    CeLog("WinMain start, build " __DATE__ " " __TIME__);

    g_hInstance = hInstance;

    g_mutex = CreateMutexW(NULL, TRUE, kMutexName);
    if (g_mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND existing = FindWindowW(kWndClassName, NULL);
        if (existing)
        {
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        CeLog("WinMain: another instance is already running, exiting");
        ExitProcess(0);
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = kWndClassName;
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_MAIN));

    if (!RegisterClassW(&wc))
    {
        CeLog("WinMain: RegisterClassW failed, error=%lu", (unsigned long)GetLastError());
        MessageBoxW(NULL, L"RegisterClassW failed", L"PopSNES", MB_OK);
        CeShutdown(1);
    }

    /* WS_POPUP (not just WS_VISIBLE): a reference sample confirmed
     * working on this exact hardware uses WS_VISIBLE | WS_POPUP for its
     * fullscreen window - a plain overlapped
     * window (the WS_VISIBLE-only style this had before) is still
     * managed by the shell as a regular window and doesn't reliably
     * reclaim the taskbar's screen space once CeHideShellChrome()
     * hides it. */
    g_hwnd = CreateWindowW(kWndClassName, L"PopSNES - No ROM loaded", WS_VISIBLE | WS_POPUP,
                            0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                            NULL, NULL, hInstance, NULL);
    if (!g_hwnd)
    {
        CeLog("WinMain: CreateWindowW failed, error=%lu", (unsigned long)GetLastError());
        MessageBoxW(NULL, L"CreateWindowW failed", L"PopSNES", MB_OK);
        CeShutdown(1);
    }

    CeHideShellChrome(g_hwnd);
    /* Re-assert visibility/layout after hiding the taskbar - the same
     * reference sample calls ShowWindow(SW_SHOWNORMAL)+UpdateWindow()
     * right after its shell-hide call even though the window was already
     * WS_VISIBLE at creation; without this the window doesn't re-layout
     * to cover the space the taskbar just vacated. */
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
    CeConfigLoad(); /* before any *_Init() below - they read their settings out of this shared table */
    CeLogSetEnabled(CeConfigGetInt("VideoDebugLog", 0)); /* Video Config's "Enable Debug Logging" checkbox - default off */
    CeLangInit(); /* loads the persisted Japanese/English UI flag; see ce_lang.h */
    CeInputInit();
    CeAudioInit();
    CeVideoInit();
    CeFileOpenInit();

    retro_set_environment(ce_environment);
    retro_set_video_refresh(ce_video_refresh);
    retro_set_audio_sample(ce_audio_sample_noop);
    retro_set_audio_sample_batch(ce_audio_sample_batch);
    retro_set_input_poll(ce_input_poll);
    retro_set_input_state(ce_input_state);

    retro_init();
    CeLog("WinMain: retro_init done");

    /* Start on the menu (matches the reference screenshot: "No ROM
     * loaded", black background) - blocks here until the user opens a
     * ROM (ShowMainMenuDialog only returns once g_romLoaded is true, or
     * the app has already exited via Exit inside the dialog). */
    ShowMainMenuDialog(g_hwnd);

    g_running = 1;
    while (g_running)
    {
        int resynced; /* g_paceResync as it was before this frame's pacing - resets the fps counter */
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                g_running = 0;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running)
            break;

        if (!g_romLoaded || g_paused)
        {
            /* No game running (still on the "No ROM loaded" screen) or
             * the touch-to-reveal menu is up: nothing to emulate/blit
             * this tick. Sleep instead of spinning the message pump at
             * 100% CPU for no reason - this device is slow enough
             * elsewhere that burning cycles here for nothing would be
             * silly. */
            Sleep(10);
            continue;
        }

        {
            static unsigned s_runAccumMs = 0, s_runMaxMs = 0, s_runCount = 0;
            DWORD t0;

            CeAudioUpdatePacing();
            if (g_audioBuffStatusCb)
            {
                /* Contractually "called right before retro_run() every
                 * frame" (see the RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_
                 * STATUS_CALLBACK doc in libretro.h) - only set while
                 * Video Config's Frame Skip is above 0 (ce_video.c), so
                 * this is a no-op call when it's Off. */
                int active, underrunLikely;
                unsigned occupancyPercent;

                if (CeVideoFrameSkipShouldForceRender())
                {
                    /* Frame Skip's cap (round 10 - see ce_video.h) has
                     * already been hit: report "buffer not active" so
                     * the core's FRAMESKIP_AUTO logic (which only skips
                     * when told the buffer is active *and* underrunning)
                     * renders this one frame regardless of the real
                     * audio state, then goes back to judging normally. */
                    active = 0;
                    occupancyPercent = 0;
                    underrunLikely = 0;
                }
                else
                {
                    CeAudioGetBufferStatus(&active, &occupancyPercent, &underrunLikely);
                }
                g_audioBuffStatusCb(active ? true : false, occupancyPercent, underrunLikely ? true : false);
            }

            t0 = GetTickCount();
            retro_run();
            {
                DWORD elapsed = (DWORD)(GetTickCount() - t0);
                s_runAccumMs += elapsed;
                if (elapsed > s_runMaxMs)
                    s_runMaxMs = elapsed;
                if (++s_runCount >= 60)
                {
                    /* retro_run() = S9xMainLoop() [CPU/APU/PPU emulation,
                     * including the ce_video_refresh -> CeDisplayBlitRGB565
                     * call, timed separately in ce_display.c's "perf: blit"
                     * line] + audio_upload_samples() [S9xMixSamples,
                     * now actually played via CeAudioPushSamples's ring
                     * buffer (CE/ce_audio.c) instead of being discarded].
                     * Comparing this line's avg against the "perf: blit"
                     * line's avg tells us how much of the frame is
                     * emulation+mixing vs. display. */
                    {
                        unsigned runAvgMs = s_runAccumMs / s_runCount;
                        unsigned runMaxMs = s_runMaxMs;
                        unsigned blitAvgMs = 0, blitMaxMs = 0;

                        CeLog("perf: retro_run avg=%ums max=%ums over %u frames (budget=17ms @60fps)",
                              runAvgMs, runMaxMs, s_runCount);

                        /* Unified summary line - same shape across every
                         * core port for side-by-side comparison. The
                         * detailed lines above (and ce_display.c's own
                         * "perf: blit" breakdown with write/BitBlt split)
                         * stay as-is; this just restates the two headline
                         * numbers on one line. blit avg/max come from the
                         * last "perf: blit" line, held live across its own
                         * 60-frame reset (see ce_display.h). */
                        CeDisplayGetLastBlitPerf(&blitAvgMs, &blitMaxMs);
                        CeLog("perf: retro_run avg=%ums max=%ums blit avg=%ums max=%ums",
                              runAvgMs, runMaxMs, blitAvgMs, blitMaxMs);
                    }
                    s_runAccumMs = 0;
                    s_runMaxMs = 0;
                    s_runCount = 0;
                }
            }
        }

        /* Frame pacing, ported from the sister PopPCE / QuickNES CE ports.
         * While a waveOut device is open, pace off the audio ring: the
         * drain thread (ce_audio.c) empties it at exactly the device's
         * playback rate, so holding the next retro_run() until the ring is
         * back down to the target locks emulation to real time. A wall
         * clock pacer on GetTickCount() alone is not enough on this device
         * (the earlier prototype found its resolution on the order of a 16ms frame;
         * here it measured ~59.9fps while the ring kept overrunning). A
         * title too heavy for full speed keeps the ring low and never
         * waits; Frame Skip's catch-up (CeAudioUpdatePacing) then skips
         * drawing until the ring is back at the target.
         *
         * Without a device (waveOutOpen failed), fall back to the wall-
         * clock pacer from g_framePeriodUs (the ROM's fps, so PAL ROMs get
         * 50fps): sleep to this frame's deadline, never wait on a late
         * frame, resync instead of racing when >100ms behind. */
        resynced = g_paceResync;
        if (CeAudioIsActive())
        {
            unsigned highMs = CeAudioGetPacingTargetMs();
            unsigned guard;

            g_paceResync = 0;
            for (guard = 0; guard < 120; guard++)
            {
                if (CeAudioGetBufferedMs() <= highMs)
                    break;
                Sleep(1);
            }
        }
        else
        {
            static DWORD s_paceDeadlineMs = 0;
            static unsigned s_paceFracUs = 0;
            DWORD now = GetTickCount();
            LONG ahead;

            if (g_paceResync)
            {
                s_paceDeadlineMs = now;
                s_paceFracUs = 0;
                g_paceResync = 0;
            }
            s_paceFracUs += g_framePeriodUs;
            s_paceDeadlineMs += s_paceFracUs / 1000u;
            s_paceFracUs %= 1000u;

            ahead = (LONG)(s_paceDeadlineMs - now);
            if (ahead > 0)
                Sleep((DWORD)ahead);
            else if (ahead < -100)
                g_paceResync = 1;
        }

        /* Measured frame rate (wall clock, emulated frames incl. skipped
         * ones) every ~5s, next to the target from retro_get_system_av_info.
         * Tells "the device can't keep up" apart from "pacing is wrong". */
        {
            static DWORD s_fpsT0 = 0;
            static unsigned s_fpsFrames = 0;
            DWORD now = GetTickCount();

            if (s_fpsT0 == 0 || resynced)
            {
                s_fpsT0 = now;
                s_fpsFrames = 0;
            }
            else if (++s_fpsFrames, now - s_fpsT0 >= 5000)
            {
                unsigned fps10 = (unsigned)((s_fpsFrames * 10000u) / (now - s_fpsT0));
                unsigned tgt10 = g_framePeriodUs ? 10000000u / g_framePeriodUs : 0;
                CeLog("perf: actual fps=%u.%u (target %u.%u) over %ums",
                      fps10 / 10, fps10 % 10, tgt10 / 10, tgt10 % 10, (unsigned)(now - s_fpsT0));
                s_fpsT0 = now;
                s_fpsFrames = 0;
            }
        }

        /* Periodic SRAM autosave (round 9) - the pause-time save in
         * ShowMainMenuDialog() only helps if the menu actually gets
         * opened before the device is powered off; this covers a
         * straight-through play session that never touches the menu at
         * all. ~30s is arbitrary (no data on this device's real
         * power-loss frequency, same caveat as the lack of a crash-safe
         * autosave noted below) - frequent enough to bound how much an
         * in-game save could be lost, infrequent enough that a `.srm`
         * write (typically a few KB) is not worth timing/skipping for. */
        {
            static DWORD s_lastSramSaveTick = 0;
            DWORD now = GetTickCount();
            if (s_lastSramSaveTick == 0)
                s_lastSramSaveTick = now; /* first frame of gameplay - start the 30s window now, not at an immediate save */
            else if (now - s_lastSramSaveTick >= 30000)
            {
                CeSaveSram();
                s_lastSramSaveTick = now;
            }
        }
    }

    CeLog("WinMain: normal shutdown");
    CeShutdown(0);
    return 0; /* unreachable - CeShutdown() calls ExitProcess() */
}
