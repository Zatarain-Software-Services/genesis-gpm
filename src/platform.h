#ifndef GENESIS_PLATFORM_H
#define GENESIS_PLATFORM_H

#include <stdio.h>

/* Shared-library visibility: genesis_runtime exports these symbols on
   Windows; consumers (CLI, LSP, DAP, extensions) import them. Empty on
   other platforms and in non-DLL builds such as the unit tests, which
   compile the sources directly. */
#if defined(_WIN32) && defined(GENESIS_RUNTIME_BUILD)
#define GENESIS_API __declspec(dllexport)
#elif defined(_WIN32) && defined(GENESIS_RUNTIME_IMPORTS)
#define GENESIS_API __declspec(dllimport)
#else
#define GENESIS_API
#endif

/* Keep the Microsoft secure CRT differences out of interpreter code. */
#ifdef _MSC_VER
#define GENESIS_FPRINTF fprintf_s
#define GENESIS_SNPRINTF(buffer, size, ...) _snprintf_s((buffer), (size), _TRUNCATE, __VA_ARGS__)
#else
#define GENESIS_FPRINTF fprintf
#define GENESIS_SNPRINTF(buffer, size, ...) snprintf((buffer), (size), __VA_ARGS__)
#endif

#endif
