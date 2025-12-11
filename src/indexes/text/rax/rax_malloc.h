/* Rax -- A radix tree implementation.
 *
 * Copyright (c) 2017, Redis Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/* Allocator selection.
 *
 * This file is used in order to change the Rax allocator at compile time.
 * Just define the following defines to what you want to use. Also add
 * the include of your alternate allocator if needed (not needed in order
 * to use the default libc allocator). */

#ifndef RAX_ALLOC_H
#define RAX_ALLOC_H
#include <stdlib.h>
#include <string.h>

// Always use manual size tracking for accurate accounting.
// malloc_usable_size() returns the actual usable size which can be larger
// than requested due to allocator overhead, causing accounting errors in rax.

static inline void* rax_malloc_impl(size_t size) {
    size_t* ptr = (size_t*)malloc(sizeof(size_t) + size);
    if (!ptr) return NULL;
    *ptr = size;
    return (void*)(ptr + 1);
}

static inline void* rax_realloc_impl(void* ptr, size_t size) {
    if (!ptr) return rax_malloc_impl(size);
    size_t* real_ptr = ((size_t*)ptr) - 1;
    size_t* new_ptr = (size_t*)realloc(real_ptr, sizeof(size_t) + size);
    if (!new_ptr) return NULL;
    *new_ptr = size;
    return (void*)(new_ptr + 1);
}

static inline void rax_free_impl(void* ptr) {
    if (!ptr) return;
    size_t* real_ptr = ((size_t*)ptr) - 1;
    free(real_ptr);
}

static inline size_t rax_ptr_alloc_size_impl(void* ptr) {
    if (!ptr) return 0;
    size_t* real_ptr = ((size_t*)ptr) - 1;
    return *real_ptr;
}

#define rax_malloc rax_malloc_impl
#define rax_realloc rax_realloc_impl
#define rax_free rax_free_impl
#define rax_ptr_alloc_size rax_ptr_alloc_size_impl

#endif
