#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct directory DIR;
struct dirent { char name[256]; };
DIR* opendir(const char* path);
/* Returns one for an entry, zero at EOF, or a negative status. */
int32_t readdir(DIR* dir, struct dirent* entry);
int32_t closedir(DIR* dir);

#ifdef __cplusplus
}
#endif
