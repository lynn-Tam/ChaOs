#pragma once

// Freestanding C string subset shared by userland libraries.
#include <libk/mem.h>

#ifdef __cplusplus
extern "C" {
#endif
char* strncpy(char*, const char*, size_t);
char* strcat(char*, const char*);
char* strncat(char*, const char*, size_t);
int strcmp(const char*, const char*);
int strncmp(const char*, const char*, size_t);
char* strrchr(const char*, int);
char* strstr(const char*, const char*);
char* strdup(const char*);
#ifdef __cplusplus
}
#endif
