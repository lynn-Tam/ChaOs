#pragma once
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_ACCMODE 3
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
#define O_APPEND 02000
#ifdef __cplusplus
extern "C" {
#endif
int open(const char* path, int flags, ...);
#ifdef __cplusplus
}
#endif
