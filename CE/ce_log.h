/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
#ifndef CE_LOG_H
#define CE_LOG_H

/* Crash-safe diagnostic logger: opens/appends/closes
 * "<exe-dir>\popsnes_debug.log" on every call. There is no debugger on
 * this device, so this (plus Windows CE's own BER crash log) is the
 * only way to see what happened after a real-hardware run.
 *
 * Gated by CeLogSetEnabled(): while disabled (the default) CeLog() is a
 * no-op and no popsnes_debug.log is created or appended. Video Config's
 * "Enable Debug Logging" checkbox is the switch, persisted as
 * "VideoDebugLog" (default 0/off). WinMain flips it on right after
 * CeConfigLoad(), so the handful of CeLog() calls before that point are
 * always suppressed. */
void CeLog(const char *fmt, ...);

void CeLogSetEnabled(int enabled);
int  CeLogIsEnabled(void);

#endif
