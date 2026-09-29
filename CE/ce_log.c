/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#include "ce_log.h"

/* Off by default - see ce_log.h. Video Config's "Enable Debug Logging"
 * checkbox (persisted as "VideoDebugLog") turns it on. */
static int s_enabled = 0;

void CeLogSetEnabled(int enabled)
{
    s_enabled = enabled ? 1 : 0;
}

int CeLogIsEnabled(void)
{
    return s_enabled;
}

void CeLog(const char *fmt, ...)
{
    wchar_t exePath[MAX_PATH];
    wchar_t logPath[MAX_PATH];
    wchar_t *slash;
    char msg[512];
    va_list ap;
    FILE *f;

    if (!s_enabled)
        return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg) - 2, fmt, ap);
    va_end(ap);

    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    slash = wcsrchr(exePath, L'\\');
    if (slash)
        *slash = L'\0';
    _snwprintf(logPath, MAX_PATH, L"%s\\popsnes_debug.log", exePath);

    f = _wfopen(logPath, L"a");
    if (!f)
        return;
    fputs(msg, f);
    fputc('\n', f);
    fclose(f);
}
