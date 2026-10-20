#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef struct directory DIR;
struct dirent { char d_name[256]; };
DIR* opendir(const char* path);
struct dirent* readdir(DIR* dir);
int closedir(DIR* dir);
#ifdef __cplusplus
}
#endif
