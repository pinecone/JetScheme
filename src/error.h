// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#ifndef error_h
#define error_h

#include <cstdio>
#include <cstdlib>

#define CITY_DIE(fmt, ...)                                                                                   \
	do                                                                                                       \
	{                                                                                                        \
		std::fprintf(stderr, fmt "\n" __VA_OPT__(, ) __VA_ARGS__);                                           \
		std::exit(1);                                                                                        \
	} while (0)

#define CITY_DIE_WHEN(cond, fmt, ...)                                                                        \
	do                                                                                                       \
	{                                                                                                        \
		if (cond) [[unlikely]]                                                                               \
		{                                                                                                    \
			CITY_DIE(fmt __VA_OPT__(, ) __VA_ARGS__);                                                        \
		}                                                                                                    \
	} while (0)

#define CITY_DIE_UNLESS(cond, fmt, ...) CITY_DIE_WHEN(!(cond), fmt __VA_OPT__(, ) __VA_ARGS__)

#endif
