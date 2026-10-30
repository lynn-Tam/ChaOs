#include <string.h>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cctype>
#include <algorithm>
#include <bit>

extern "C" char* strncpy(char* d, const char* s, size_t n) { size_t i = 0; while (i < n && s[i]) { d[i] = s[i]; ++i; } if (i < n) ::memset(d + i, 0, n - i); return d; }
extern "C" char* strcat(char* d, const char* s) { ::strcpy(d + ::strlen(d), s); return d; }
extern "C" char* strncat(char* d, const char* s, size_t n) { auto* p = d + ::strlen(d); while (n-- && *s) *p++ = *s++; *p = 0; return d; }
extern "C" int strcmp(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return (unsigned char)*a - (unsigned char)*b; }
extern "C" int strncmp(const char* a, const char* b, size_t n) { while (n && *a && *a == *b) { ++a; ++b; --n; } return n ? (unsigned char)*a - (unsigned char)*b : 0; }
extern "C" int strcasecmp(const char* a, const char* b) { while (*a && std::tolower((unsigned char)*a) == std::tolower((unsigned char)*b)) { ++a; ++b; } return std::tolower((unsigned char)*a) - std::tolower((unsigned char)*b); }
extern "C" int strncasecmp(const char* a, const char* b, size_t n) { while (n && *a && std::tolower((unsigned char)*a) == std::tolower((unsigned char)*b)) { ++a; ++b; --n; } return n ? std::tolower((unsigned char)*a) - std::tolower((unsigned char)*b) : 0; }
extern "C" char* strrchr(const char* s, int c) { const char* found = nullptr; do { if (*s == char(c)) found = s; } while (*s++); return const_cast<char*>(found); }
extern "C" char* strstr(const char* s, const char* needle) { const auto n = ::strlen(needle); if (!n) return const_cast<char*>(s); for (; *s; ++s) if (!::strncmp(s, needle, n)) return const_cast<char*>(s); return nullptr; }
extern "C" char* strdup(const char* s) { const auto n = ::strlen(s) + 1; auto* p = static_cast<char*>(::malloc(n)); if (p) ::memcpy(p, s, n); return p; }

// Numeric parsing shares the freestanding standard library's checked core.
static auto px_integer(const char* text, char** end, int base, bool sign) -> unsigned long {
    const char* p = text; while (std::isspace((unsigned char)*p)) ++p;
    const bool negative = *p == '-'; if (*p == '-' || *p == '+') ++p;
    if (base && (base < 2 || base > 36)) { errno = EINVAL; if (end) *end = const_cast<char*>(text); return 0; }
    const auto digit = [](unsigned char c) { if (c >= '0' && c <= '9') return c - '0'; c = std::tolower(c); return c >= 'a' && c <= 'z' ? c - 'a' + 10 : 36; };
    if ((!base || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && digit(p[2]) < 16) { base = 16; p += 2; }
    if (!base) base = *p == '0' ? 8 : 10;
    auto* last = p; while (digit((unsigned char)*last) < base) ++last;
    unsigned long value{}; auto r = std::from_chars(p, last, value, base);
    if (end) *end = const_cast<char*>(last == p ? text : last);
    const auto max = sign ? (unsigned long)LONG_MAX + negative : ULONG_MAX;
    if (r.ec == std::errc::result_out_of_range || value > max) { errno = ERANGE; return sign && negative ? 0UL - max : max; }
    return negative ? 0UL - value : value;
}
extern "C" unsigned long strtoul(const char* s, char** end, int base) { return px_integer(s, end, base, false); }
extern "C" long strtol(const char* s, char** end, int base) { return std::bit_cast<long>(px_integer(s, end, base, true)); }
extern "C" int atoi(const char* s) { return strtol(s, nullptr, 10); }
extern "C" long atol(const char* s) { return strtol(s, nullptr, 10); }
extern "C" double atof(const char* s) { double value{}; (void)sscanf(s, "%lf", &value); return value; }
extern "C" int abs(int n) { return n < 0 ? -n : n; }
extern "C" long labs(long n) { return n < 0 ? -n : n; }

// C locale, matching this adapter's selected printf/scanf locale.
extern "C" int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
extern "C" int isdigit(int c) { return c >= '0' && c <= '9'; }
extern "C" int isalpha(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
extern "C" int isalnum(int c) { return isalpha(c) || isdigit(c); }
extern "C" int isprint(int c) { return c >= 32 && c <= 126; }
extern "C" int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
extern "C" int isupper(int c) { return c >= 'A' && c <= 'Z'; }
extern "C" int islower(int c) { return c >= 'a' && c <= 'z'; }
extern "C" int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
extern "C" int tolower(int c) { return isupper(c) ? c + ('a' - 'A') : c; }
extern "C" int toupper(int c) { return islower(c) ? c - ('a' - 'A') : c; }

extern "C" char** environ;
extern "C" char* getenv(const char* name) {
    if (!name || !*name || ::strchr(name, '=')) return nullptr;
    const auto n = ::strlen(name);
    for (auto** p = environ; p && *p; ++p) if (!::strncmp(*p, name, n) && (*p)[n] == '=') return *p + n + 1;
    return nullptr;
}
extern "C" int isblank(int c) { return c == ' ' || c == '\t'; }
extern "C" int isgraph(int c) { return c >= 33 && c <= 126; }
extern "C" int ispunct(int c) { return isgraph(c) && !isalnum(c); }
