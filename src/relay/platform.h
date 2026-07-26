#ifndef RELAY_PLATFORM_H
#define RELAY_PLATFORM_H

#if defined(_WIN32)
#define RELAY_PLATFORM_WINDOWS 1
#define RELAY_PLATFORM_MACOS 0
#define RELAY_PLATFORM_LINUX 0
#define RELAY_PLATFORM_NAME "Windows"
#elif defined(__APPLE__) && defined(__MACH__)
#define RELAY_PLATFORM_WINDOWS 0
#define RELAY_PLATFORM_MACOS 1
#define RELAY_PLATFORM_LINUX 0
#define RELAY_PLATFORM_NAME "macOS"
#elif defined(__linux__)
#define RELAY_PLATFORM_WINDOWS 0
#define RELAY_PLATFORM_MACOS 0
#define RELAY_PLATFORM_LINUX 1
#define RELAY_PLATFORM_NAME "Linux"
#else
#error "Relay supports Windows, macOS, and Linux only."
#endif

#endif
