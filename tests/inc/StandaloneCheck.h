#pragma once
#include <cstdio>
#include <cstdlib>

// Standalone integration checks must run in Release builds too.
#define CHECK(condition) do { \
	if (!(condition)) { \
		std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		std::abort(); \
	} \
} while (false)
