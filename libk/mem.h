#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
int memcmp(const void* lhs, const void* rhs, size_t n);

#ifdef __cplusplus
}
#endif
