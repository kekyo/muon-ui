/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once
#include <math.h>

/* glibc 2.35 changed the default symbol for hypot. Bind QuickJS's Math.hypot
 * to the libm symbol available on each supported architecture in glibc 2.34. */
#if defined(__linux__) && defined(__GLIBC__)
#if defined(__x86_64__) && defined(__LP64__)
__asm__(".symver hypot, hypot@GLIBC_2.2.5");
#elif defined(__aarch64__) && defined(__LP64__)
__asm__(".symver hypot, hypot@GLIBC_2.17");
#elif defined(__arm__) && defined(__ARM_PCS_VFP)
__asm__(".symver hypot, hypot@GLIBC_2.4");
#endif
#endif
