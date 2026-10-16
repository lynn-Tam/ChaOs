#include <util.hpp>
#include <algorithm>
#include <cstddef>
#include <new>
#include <string>
#include <start.hpp>
#include <cap.hpp>
#include <string.h>


// Process-local runtime: allocation and mapping mutation run on its one thread.
constexpr size_t Align = alignof(std::max_align_t);
constexpr size_t ChunkSize = 64 * 1024;
struct Chunk;
struct alignas(std::max_align_t) Block {
    size_t size;
    Block *prev, *next;
    Chunk* chunk;
    bool free;
};
struct Chunk {
    vm_area map;
    Chunk *prev, *next;
};
struct Alloc { Block* block; size_t size; };
static constexpr auto round(size_t n, size_t a) noexcept -> size_t { return (n + a - 1) & ~(a - 1); }
constexpr size_t Header = round(sizeof(Chunk), Align);
static Chunk* chunks;
static cap_t pool, vm;

static auto allocate(size_t n, size_t alignment = Align) noexcept -> void* {
    if (!pool || !alignment || (alignment & (alignment - 1))) return nullptr;
    alignment = std::max(alignment, Align);
    if (alignment > SIZE_MAX - sizeof(Block) - sizeof(Alloc) - Align
        || n > SIZE_MAX - alignment - sizeof(Block) - sizeof(Alloc) - Align) return nullptr;
    const auto need = round(sizeof(Block) + sizeof(Alloc) + alignment - 1 + std::max(n, size_t{1}), Align);
    Block* block{};
    for (auto* c = chunks; c && !block; c = c->next)
        for (auto* b = reinterpret_cast<Block*>(reinterpret_cast<char*>(c) + Header); b; b = b->next)
            if (b->free && b->size >= need) { block = b; break; }
    if (!block) {
        if (need > SIZE_MAX - Header - 4095) return nullptr;
        const auto minimum = round(Header + need, 4096);
        auto size = std::max(ChunkSize, minimum);
        // Return only backed storage. Failed reservations release through RAII;
        // growth granularity must not reject a smaller admissible allocation.
        auto map = vm_area::create(pool, vm, 0, size);
        if (!map && size != minimum && map.error() == STATUS_NO_MEMORY) {
            size = minimum;
            map = vm_area::create(pool, vm, 0, size);
        }
        if (!map) return nullptr;
        auto* c = std::construct_at(reinterpret_cast<Chunk*>(map->address), std::move(*map), nullptr, chunks);
        if (chunks) chunks->prev = c;
        chunks = c;
        block = std::construct_at(reinterpret_cast<Block*>(reinterpret_cast<char*>(c) + Header),
            size - Header, nullptr, nullptr, c, true);
    }
    if (block->size - need >= sizeof(Block) + Align) {
        auto* tail = std::construct_at(reinterpret_cast<Block*>(reinterpret_cast<char*>(block) + need),
            block->size - need, block, block->next, block->chunk, true);
        if (block->next) block->next->prev = tail;
        block->next = tail; block->size = need;
    }
    block->free = false;
    const auto address = round(reinterpret_cast<uintptr_t>(block) + sizeof(Block) + sizeof(Alloc), alignment);
    std::construct_at(reinterpret_cast<Alloc*>(address) - 1, block, n);
    return reinterpret_cast<void*>(address);
}
static void merge(Block& b) noexcept {
    auto* next = b.next;
    b.size += next->size; b.next = next->next;
    if (b.next) b.next->prev = &b;
}
static void release(void* p) noexcept {
    if (!p) return;
    auto* b = (static_cast<Alloc*>(p) - 1)->block;
    b->free = true;
    if (b->next && b->next->free) merge(*b);
    if (b->prev && b->prev->free) { b = b->prev; merge(*b); }
    if (b->prev || b->next) return;
    auto* c = b->chunk;
    if (c->prev) c->prev->next = c->next;
    else chunks = c->next;
    if (c->next) c->next->prev = c->prev;
    // Move the reservation off the memory which its destructor will unmap.
    auto map = std::move(c->map);
    std::destroy_at(c);
}
static auto checked(size_t n, size_t alignment = Align) -> void* {
    if (auto* p = allocate(n, alignment)) return p;
    sys::exit(STATUS_NO_MEMORY);
}


extern "C" void init_heap(cap_t pool_cap, cap_t vm_cap) noexcept {
    pool = pool_cap; vm = vm_cap;
}
extern "C" auto malloc(size_t n) noexcept -> void* { return allocate(n); }
extern "C" void free(void* p) noexcept { release(p); }
extern "C" auto calloc(size_t count, size_t size) noexcept -> void* {
    if (count && size > SIZE_MAX / count) return nullptr;
    const auto n = count * size;
    auto* p = allocate(n);
    if (p) ::memset(p, 0, n);
    return p;
}
extern "C" auto realloc(void* p, size_t n) noexcept -> void* {
    if (!p) return allocate(n);
    if (!n) { release(p); return nullptr; }
    auto* result = allocate(n);
    if (result) { ::memcpy(result, p, std::min(n, (static_cast<Alloc*>(p) - 1)->size)); release(p); }
    return result;
}
extern "C" auto aligned_alloc(size_t a, size_t n) noexcept -> void* {
    return a && n % a == 0 ? allocate(n, a) : nullptr;
}
void* operator new(size_t n) { return checked(n); }
void* operator new[](size_t n) { return checked(n); }
void* operator new(size_t n, std::align_val_t a) { return checked(n, static_cast<size_t>(a)); }
void* operator new[](size_t n, std::align_val_t a) { return checked(n, static_cast<size_t>(a)); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return allocate(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return allocate(n); }
void* operator new(size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return allocate(n, static_cast<size_t>(a)); }
void* operator new[](size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return allocate(n, static_cast<size_t>(a)); }
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, size_t) noexcept { release(p); }
void operator delete[](void* p, size_t) noexcept { release(p); }
void operator delete(void* p, std::align_val_t) noexcept { release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { release(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { release(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { release(p); }
const std::nothrow_t std::nothrow{};
[[noreturn]] void std::__throw_bad_alloc() { sys::exit(STATUS_NO_MEMORY); }
[[noreturn]] void std::__throw_bad_array_new_length() { sys::exit(STATUS_NO_MEMORY); }
[[noreturn]] void std::__throw_logic_error(const char*) { sys::exit(STATUS_BAD_ARGS); }
[[noreturn]] void std::__throw_length_error(const char*) { sys::exit(STATUS_BAD_ARGS); }
[[noreturn]] void std::__throw_out_of_range_fmt(const char*, ...) { sys::exit(STATUS_BAD_ARGS); }

// libstdc++ declares this cold path extern even in the freestanding headers.
// Instantiate its supplied definition so string overlap keeps STL semantics.
template void std::basic_string<char>::_M_replace_cold(char*, size_type, const char*, size_type, size_type);
