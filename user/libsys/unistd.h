#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
#define O_APPEND 02000

int32_t open(const char* path, uint32_t flags);
int32_t close(int32_t fd);
int32_t dup(int32_t fd);
int64_t read(int32_t fd, void* data, uint64_t size);
int64_t write(int32_t fd, const void* data, uint64_t size);
int64_t pread(int32_t fd, void* data, uint64_t size, uint64_t offset);
int64_t pwrite(int32_t fd, const void* data, uint64_t size, uint64_t offset);
int32_t fsync(int32_t fd);
// Sync the authorized writable volume; zero on success, negative status on failure.
int32_t sync(void);
int32_t unlink(const char* path);
int32_t rename(const char* from, const char* to);
// Replace the trailing XXXXXX and create an exclusive read/write file.
int32_t mkstemp(char* path);

#ifdef __cplusplus
}
#endif
