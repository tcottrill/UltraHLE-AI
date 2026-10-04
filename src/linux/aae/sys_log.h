// AAE's logging macros for the files taken from AAE, written to UltraHLE's
// startup log (bootlog.h: startup.log and stderr).
#pragma once

#include "../../bootlog.h"

#define LOG_INFO(...)  bootlog(__VA_ARGS__)
#define LOG_WARN(...)  bootlog(__VA_ARGS__)
#define LOG_ERROR(...) bootlog(__VA_ARGS__)
#define LOG_DEBUG(...) ((void)0)

// gamepad slots: also in ultra.log, the log users send
extern "C" void print(const char* txt, ...);
#define LOG_PAD(...)   (bootlog(__VA_ARGS__), print(__VA_ARGS__), print("\n"))
