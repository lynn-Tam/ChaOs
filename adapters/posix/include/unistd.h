#pragma once
#include <sys/types.h>
#include <fcntl.h>
#ifdef __cplusplus
extern "C" {
#endif
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
int open(const char* path, int flags, ...);
int close(int fd);
int dup(int fd);
ssize_t read(int fd, void* data, size_t size);
ssize_t write(int fd, const void* data, size_t size);
ssize_t pread(int fd, void* data, size_t size, off_t offset);
ssize_t pwrite(int fd, const void* data, size_t size, off_t offset);
int fsync(int fd);
int unlink(const char* path);
int rename(const char* from, const char* to);
int mkstemp(char* path);
#ifdef __cplusplus
}
#endif
