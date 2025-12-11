/*
 * Copyright (c) 2025, valkey-search contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 */

#ifndef VALKEY_SEARCH_SERVERASSERT_H_
#define VALKEY_SEARCH_SERVERASSERT_H_

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

// Stub header to satisfy rax.c dependencies
// Maps Valkey server assertions to standard C assertions

#define serverAssert(cond) assert(cond)
#define serverAssertWithInfo(c, o, cond) assert(cond)
#define serverPanic(...) do { fprintf(stderr, __VA_ARGS__); abort(); } while(0)

#endif  // VALKEY_SEARCH_SERVERASSERT_H_
