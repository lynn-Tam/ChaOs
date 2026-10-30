#include <libk/mem.h>
#include <stdint.h>

// Aligned scalar accesses only: the same implementation is used before traps
// are available and on RV64IMAC, without an unaligned-access or SIMD contract.
typedef uintptr_t mem_word __attribute__((__may_alias__));
typedef uint32_t mem_u32 __attribute__((__may_alias__));

void* memset(void* dst, int c, size_t n) {
    unsigned char* p = (unsigned char*)dst;
    const unsigned char byte = (unsigned char)c;
    while (n && (uintptr_t)p % sizeof(mem_word)) { *p++ = byte; --n; }
    const mem_word value = (mem_word)-1 / 255 * byte;
    while (n >= sizeof(mem_word)) { *(mem_word*)p = value; p += sizeof(mem_word); n -= sizeof(mem_word); }
    while (n--) *p++ = byte;
    return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (((uintptr_t)d ^ (uintptr_t)s) % sizeof(mem_u32) == 0) {
        while (n && (uintptr_t)d % sizeof(mem_u32)) { *d++ = *s++; --n; }
        if (((uintptr_t)d | (uintptr_t)s) % sizeof(mem_word) == 0) {
            while (n >= sizeof(mem_word)) {
                *(mem_word*)d = *(const mem_word*)s;
                d += sizeof(mem_word); s += sizeof(mem_word); n -= sizeof(mem_word);
            }
        }
        while (n >= sizeof(mem_u32)) {
            *(mem_u32*)d = *(const mem_u32*)s;
            d += sizeof(mem_u32); s += sizeof(mem_u32); n -= sizeof(mem_u32);
        }
    }
    while (n--) *d++ = *s++;
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

void* memchr(const void* data, int c, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i)
        if (p[i] == (unsigned char)c) return (void*)(p + i);
    return 0;
}

size_t strlen(const char* text) {
    size_t size = 0;
    while (text[size] != '\0') ++size;
    return size;
}

char* strcpy(char* dst, const char* src) {
    char* out = dst;
    while ((*dst++ = *src++) != '\0') {}
    return out;
}

char* strchr(const char* text, int character) {
    const char value = (char)character;
    while (*text != value) {
        if (*text == '\0') return 0;
        ++text;
    }
    return (char*)text;
}

size_t strspn(const char* text, const char* accept) {
    size_t count = 0;
    for (; text[count] != '\0'; ++count) {
        const char* match = accept;
        while (*match != '\0' && *match != text[count]) ++match;
        if (*match == '\0') break;
    }
    return count;
}

size_t strcspn(const char* text, const char* reject) {
    size_t count = 0;
    for (; text[count] != '\0'; ++count) {
        const char* match = reject;
        while (*match != '\0' && *match != text[count]) ++match;
        if (*match != '\0') break;
    }
    return count;
}
