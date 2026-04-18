#pragma once

// Stringify helpers.
#define _CAART_STR(x) #x
#define CAART_STR(x)  _CAART_STR(x)

// Major version — increment manually for breaking/significant changes.
#define PLUGIN_VERSION_MAJOR 1

// BUILD_NUMBER is injected by CI as -DBUILD_NUMBER=<git-commit-count>.
// Local builds omit it; the version string degrades gracefully to just "1".
#ifdef BUILD_NUMBER
#  define PLUGIN_VERSION_STRING CAART_STR(PLUGIN_VERSION_MAJOR) "." CAART_STR(BUILD_NUMBER)
#else
#  define PLUGIN_VERSION_STRING CAART_STR(PLUGIN_VERSION_MAJOR)
#endif

// Numeric form returned by driverInfoVersion() (TheSkyX expects a double).
#define PLUGIN_VERSION_DOUBLE  1.0

// Git commit hash injected at build time via -DGIT_HASH=\"...\".
#ifndef GIT_HASH
#define GIT_HASH "unknown"
#endif
