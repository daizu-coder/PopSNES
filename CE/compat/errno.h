/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Stand-in for <errno.h> on this cegcc toolchain.
 *
 * /opt/cegcc/arm-mingw32ce/include/errno.h does `#include_next <errno.h>`
 * to hand off to coredll's own errno.h, but there is no further directory
 * in the search chain that provides one, so the include_next fails ("no
 * include path in which to search for errno.h"). Nothing in this codebase
 * reads/writes `errno` (verified by grep - soundux.c includes it but never
 * uses the symbol), so an empty header found first via -Icompat is enough.
 */
#ifndef CE_COMPAT_ERRNO_H
#define CE_COMPAT_ERRNO_H
#endif
