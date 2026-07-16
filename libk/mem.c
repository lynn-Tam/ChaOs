#include <libk/mem.h>

void* memset(void* dst, int c, size_t n) {
    unsigned char* p = (unsigned char*)dst;
    const unsigned char byte = (unsigned char)c;
    for (size_t i = 0; i < n; ++i) {
        p[i] = byte;
    }
    return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (size_t i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;

    if (d == s || n == 0) {
        return dst;
    }

    if (d < s) {
        for (size_t i = 0; i < n; ++i) {
            d[i] = s[i];
        }
        return dst;
    }

    for (size_t i = n; i != 0; --i) {
        d[i - 1] = s[i - 1];
    }
    return dst;
}

int memcmp(const void* lhs, const void* rhs, size_t n) {
    const unsigned char* a = (const unsigned char*)lhs;
    const unsigned char* b = (const unsigned char*)rhs;
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            return (a[i] < b[i]) ? -1 : 1;
        }
    }
    return 0;
}
