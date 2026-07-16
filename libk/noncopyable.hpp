#pragma once
namespace libk {
struct noncopyable {
protected:
    noncopyable() = default;
    ~noncopyable() = default;

    noncopyable(const noncopyable&) = delete;
    noncopyable& operator=(const noncopyable&) = delete;

    noncopyable(noncopyable&&) noexcept = default;
    noncopyable& operator=(noncopyable&&) noexcept = default;
};

struct nonmovable {
protected:
    nonmovable() = default;
    ~nonmovable() = default;

    nonmovable(const nonmovable&) = default;
    nonmovable& operator=(const nonmovable&) = default;

    nonmovable(nonmovable&&) = delete;
    nonmovable& operator=(nonmovable&&) = delete;
};

struct noncopyable_nonmovable {
protected:
    noncopyable_nonmovable() = default;
    ~noncopyable_nonmovable() = default;

    noncopyable_nonmovable(const noncopyable_nonmovable&) = delete;
    noncopyable_nonmovable& operator=(const noncopyable_nonmovable&) = delete;

    noncopyable_nonmovable(noncopyable_nonmovable&&) = delete;
    noncopyable_nonmovable& operator=(noncopyable_nonmovable&&) = delete;
};
}// namespace
