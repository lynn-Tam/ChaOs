#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct stat { uint64_t size; };
int32_t fstat(int32_t fd, struct stat* info);
int32_t mkdir(const char* path);
#ifdef __cplusplus
}
#endif
