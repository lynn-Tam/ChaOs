#include <stdio.h>
#include <limits.h>
#include <stdint.h>
#define stbsp__uintptr uintptr_t
#define STB_SPRINTF_NOUNALIGNED
#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>
#define SCANF_NODEFINE
#include <scanf.h>

int vsnprintf(char* s, size_t n, const char* fmt, va_list ap) { return stbsp_vsnprintf(s, n > INT_MAX ? INT_MAX : (int)n, fmt, ap); }
int snprintf(char* s, size_t n, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(s, n, fmt, ap); va_end(ap); return r; }
int vsprintf(char* s, const char* fmt, va_list ap) { return stbsp_vsprintf(s, fmt, ap); }
int sprintf(char* s, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsprintf(s, fmt, ap); va_end(ap); return r; }
struct px_format { FILE* file; int failed; char data[STB_SPRINTF_MIN]; };
static char* px_formatted(const char* bytes, void* context, int length) {
    struct px_format* out = context;
    if (fwrite(bytes, 1, length, out->file) != (size_t)length) { out->failed = 1; return NULL; }
    return out->data;
}
int vfprintf(FILE* f, const char* fmt, va_list ap) { struct px_format out = {f, 0, {0}}; int n = stbsp_vsprintfcb(px_formatted, &out, out.data, fmt, ap); return out.failed ? -1 : n; }
int fprintf(FILE* f, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap); return r; }
int vprintf(const char* fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }
int printf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vprintf(fmt, ap); va_end(ap); return r; }
int vsscanf(const char* s, const char* fmt, va_list ap) { return vsscanf_(s, fmt, ap); }
int sscanf(const char* s, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsscanf(s, fmt, ap); va_end(ap); return r; }
static int px_getch(void* f) { return fgetc(f); }
static void px_ungetch(int c, void* f) { (void)ungetc(c, f); }
int vfscanf(FILE* f, const char* fmt, va_list ap) { return vfctscanf_(px_getch, px_ungetch, f, fmt, ap); }
int fscanf(FILE* f, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfscanf(f, fmt, ap); va_end(ap); return r; }
int vscanf(const char* fmt, va_list ap) { return vfscanf(stdin, fmt, ap); }
int scanf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vscanf(fmt, ap); va_end(ap); return r; }

int getch_(void) { return getchar(); }
void ungetch_(int c) { (void)ungetc(c, stdin); }
