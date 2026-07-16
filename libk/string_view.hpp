#pragma once

#include <stddef.h>

#include <libk/assert.hpp>

namespace libk {

class StrView final {
public:
    constexpr StrView() noexcept = default;

    constexpr StrView(const char* data, size_t size) noexcept
        : data_(data), size_(size) {
        libk_assert(data_ != nullptr || size_ == 0);
    }

    template<size_t N>
    constexpr StrView(const char (&text)[N]) noexcept
        : data_(text), size_(N - 1) {
        static_assert(N != 0);
        libk_assert(text[N - 1] == '\0');
    }

    [[nodiscard]] constexpr auto data() const noexcept -> const char* {
        return data_;
    }
    [[nodiscard]] constexpr auto size() const noexcept -> size_t {
        return size_;
    }
    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return size_ == 0;
    }
    [[nodiscard]] constexpr auto begin() const noexcept -> const char* {
        return data_;
    }
    [[nodiscard]] constexpr auto end() const noexcept -> const char* {
        return data_ == nullptr ? nullptr : data_ + size_;
    }

    [[nodiscard]] constexpr auto starts_with(StrView prefix) const noexcept
        -> bool {
        if (prefix.size_ > size_) {
            return false;
        }
        for (size_t index = 0; index < prefix.size_; ++index) {
            if (data_[index] != prefix.data_[index]) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] constexpr auto substr(
        size_t offset,
        size_t count) const noexcept -> StrView {
        libk_assert(offset <= size_);
        libk_assert(count <= size_ - offset);
        return data_ == nullptr
            ? StrView{}
            : StrView{data_ + offset, count};
    }

    [[nodiscard]] constexpr auto substr(size_t offset) const noexcept
        -> StrView {
        libk_assert(offset <= size_);
        return substr(offset, size_ - offset);
    }

    [[nodiscard]] constexpr auto operator[](size_t index) const noexcept
        -> char {
        libk_assert(index < size_);
        return data_[index];
    }

    [[nodiscard]] static constexpr auto from_cstr(const char* text) noexcept
        -> StrView {
        libk_assert(text != nullptr);
        size_t size{};
        while (text[size] != '\0') {
            ++size;
        }
        return StrView{text, size};
    }

    [[nodiscard]] friend constexpr auto operator==(
        StrView lhs,
        StrView rhs) noexcept -> bool {
        if (lhs.size_ != rhs.size_) {
            return false;
        }
        for (size_t index = 0; index < lhs.size_; ++index) {
            if (lhs.data_[index] != rhs.data_[index]) {
                return false;
            }
        }
        return true;
    }

private:
    const char* data_{};
    size_t size_{};
};

} // namespace libk
