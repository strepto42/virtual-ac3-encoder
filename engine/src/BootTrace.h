// BootTrace.h — temporary start-up diagnostics. Each VAC3_TRACE line goes to the engine log
// (unbuffered, so it survives a hang or a crash) and shows how far start-up got.
// Set VAC3_BOOT_TRACE to 0 (compile definition or here) to silence them once everything works.
#pragma once

#include <cstdio>

#ifndef VAC3_BOOT_TRACE
#define VAC3_BOOT_TRACE 1
#endif

#if VAC3_BOOT_TRACE
#define VAC3_TRACE(...) do { std::printf(__VA_ARGS__); std::fflush(stdout); } while (0)
#else
#define VAC3_TRACE(...) do { } while (0)
#endif
