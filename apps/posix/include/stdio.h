#pragma once
#include <stddef.h>
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct px_file FILE;
#define __FILE_defined
typedef long fpos_t;
#define EOF (-1)
#define BUFSIZ 4096
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#define FOPEN_MAX 64
#define FILENAME_MAX 4096
#define L_tmpnam 64
extern FILE *stdin, *stdout, *stderr;
FILE* fopen(const char*, const char*);
FILE* fdopen(int, const char*);
FILE* freopen(const char*, const char*, FILE*);
int fclose(FILE*);
int fflush(FILE*);
size_t fread(void*, size_t, size_t, FILE*);
size_t fwrite(const void*, size_t, size_t, FILE*);
int fseek(FILE*, long, int);
long ftell(FILE*);
int fgetpos(FILE*, fpos_t*);
int fsetpos(FILE*, const fpos_t*);
void rewind(FILE*);
int fgetc(FILE*);
int getc(FILE*);
int getchar(void);
char* fgets(char*, int, FILE*);
int fputc(int, FILE*);
int putc(int, FILE*);
int putchar(int);
int fputs(const char*, FILE*);
int puts(const char*);
int ungetc(int, FILE*);
int feof(FILE*);
int ferror(FILE*);
void clearerr(FILE*);
int fileno(FILE*);
void setbuf(FILE*, char*);
int setvbuf(FILE*, char*, int, size_t);
int vfprintf(FILE*, const char*, va_list);
int fprintf(FILE*, const char*, ...);
int vprintf(const char*, va_list);
int printf(const char*, ...);
int vsnprintf(char*, size_t, const char*, va_list);
int snprintf(char*, size_t, const char*, ...);
int vsprintf(char*, const char*, va_list);
int sprintf(char*, const char*, ...);
int vsscanf(const char*, const char*, va_list);
int sscanf(const char*, const char*, ...);
int vfscanf(FILE*, const char*, va_list);
int fscanf(FILE*, const char*, ...);
int vscanf(const char*, va_list);
int scanf(const char*, ...);
int remove(const char*);
int rename(const char*, const char*);
void perror(const char*);
FILE* tmpfile(void);
char* tmpnam(char*);
#ifdef __cplusplus
}
#endif
