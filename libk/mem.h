#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
int memcmp(const void* lhs, const void* rhs, size_t n);
size_t strlen(const char* text);
char* strcpy(char* dst, const char* src);
char* strchr(const char* text, int character);
size_t strspn(const char* text, const char* accept);
size_t strcspn(const char* text, const char* reject);

#ifdef __cplusplus
}
#endif
