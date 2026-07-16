#pragma once

#include <stdint.h>
#include <stddef.h>

// Fixed-width unsigned integers
using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// Fixed-width signed integers
using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

// Pointer-sized integers
using usize = size_t;
using isize = ptrdiff_t;

using uptr = uintptr_t;
using iptr = intptr_t;

// Raw byte type.
//
// 用 u8 做 byte 比 std::byte 更适合早期 kernel：
// - 不依赖 C++ 标准库语义
// - 可以直接参与位运算
// - 可以直接表示内存内容
using byte = u8;

// Address-sized raw integer types.
//
// 注意：这只是“地址整数”，不是强类型地址对象。
// Common constants
constexpr usize KiB = 1024;
constexpr usize MiB = 1024 * KiB;
constexpr usize GiB = 1024 * MiB;

// Compile-time sanity checks
static_assert(sizeof(u8)  == 1);
static_assert(sizeof(u16) == 2);
static_assert(sizeof(u32) == 4);
static_assert(sizeof(u64) == 8);

static_assert(sizeof(i8)  == 1);
static_assert(sizeof(i16) == 2);
static_assert(sizeof(i32) == 4);
static_assert(sizeof(i64) == 8);

static_assert(sizeof(uptr) == sizeof(void*));
static_assert(sizeof(iptr) == sizeof(void*));
static_assert(sizeof(usize) == sizeof(void*));
