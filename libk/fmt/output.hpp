#pragma once

#include <stddef.h>

#include "libk/concepts.hpp"
#include "libk/string_view.hpp"
#include "libk/fmt/error.hpp"

namespace libk::fmt {

template<size_t N>
class fixed_buffer {
    static_assert(N > 0, "libk::fmt::fixed_buffer<N> requires N > 0");
public:
    constexpr fixed_buffer() = default;

    constexpr bool write(char c) noexcept {
        ++attempted_;
        if (size_ >= N) {
            truncated_ = true;
            return false;
        }
        data_[size_++] = c;
        return true;
    }

    constexpr bool write(const char* text, size_t n) noexcept {
        attempted_ += n;
        size_t copied = 0;
        while (copied < n && size_ < N) {
            data_[size_++] = text[copied++];
        }
        if (copied != n) {
            truncated_ = true;
            return false;
        }
        return true;
    }

    constexpr void clear() noexcept {
        size_ = 0;
        attempted_ = 0;
        truncated_ = false;
    }

    constexpr char* data() noexcept { return data_; }
    constexpr const char* data() const noexcept { return data_; }
    constexpr size_t size() const noexcept { return size_; }
    constexpr size_t capacity() const noexcept { return N; }
    constexpr size_t attempted() const noexcept { return attempted_; }
    constexpr bool truncated() const noexcept { return truncated_; }
    constexpr StrView view() const noexcept { return StrView{data_, size_}; }

    constexpr bool null_terminate() noexcept {
        if (N == 0) {
            return false;
        }
        if (size_ < N) {
            data_[size_] = '\0';
            return true;
        }
        data_[N - 1] = '\0';
        truncated_ = true;
        return false;
    }

private:
    char data_[N]{};
    size_t size_{};
    size_t attempted_{};
    bool truncated_{};
};

class span_buffer {
public:
    constexpr span_buffer(char* data, size_t capacity) noexcept
        : data_(data), capacity_(capacity) {}

    constexpr bool write(char c) noexcept {
        ++attempted_;
        if (size_ >= capacity_) {
            truncated_ = true;
            return false;
        }
        data_[size_++] = c;
        return true;
    }

    constexpr bool write(const char* text, size_t n) noexcept {
        attempted_ += n;
        size_t copied = 0;
        while (copied < n && size_ < capacity_) {
            data_[size_++] = text[copied++];
        }
        if (copied != n) {
            truncated_ = true;
            return false;
        }
        return true;
    }

    constexpr char* data() noexcept { return data_; }
    constexpr const char* data() const noexcept { return data_; }
    constexpr size_t size() const noexcept { return size_; }
    constexpr size_t capacity() const noexcept { return capacity_; }
    constexpr size_t attempted() const noexcept { return attempted_; }
    constexpr bool truncated() const noexcept { return truncated_; }
    constexpr StrView view() const noexcept { return StrView{data_, size_}; }

    constexpr bool null_terminate() noexcept {
        if (capacity_ == 0) {
            truncated_ = true;
            return false;
        }
        if (size_ < capacity_) {
            data_[size_] = '\0';
            return true;
        }
        data_[capacity_ - 1] = '\0';
        truncated_ = true;
        return false;
    }

private:
    char* data_{};
    size_t capacity_{};
    size_t size_{};
    size_t attempted_{};
    bool truncated_{};
};

class counting_sink {
public:
    constexpr bool write(char) noexcept { return true; }
    constexpr bool write(const char*, size_t) noexcept { return true; }
};

namespace detail {

template<typename R>
constexpr bool sink_result_ok(R value) noexcept {
    if constexpr (SameAs<R, bool>) {
        return value;
    } else if constexpr (SameAs<R, errc>) {
        return value == errc::ok;
    } else {
        return static_cast<bool>(value);
    }
}

template<typename Sink>
constexpr bool write_one_to_sink(Sink& sink, char c) noexcept {
    if constexpr (requires(Sink& s, char ch) { s.write(ch); }) {
        using R = decltype(sink.write(c));
        if constexpr (is_void_v<R>) {
            sink.write(c);
            return true;
        } else {
            return sink_result_ok(sink.write(c));
        }
    } else if constexpr (requires(Sink& s, char ch) { s.put(ch); }) {
        using R = decltype(sink.put(c));
        if constexpr (is_void_v<R>) {
            sink.put(c);
            return true;
        } else {
            return sink_result_ok(sink.put(c));
        }
    } else {
        static_assert(sizeof(Sink) == 0,
            "fmt sink must provide write(char), put(char), or write(const char*, size_t)");
        return false;
    }
}

template<typename Sink>
constexpr bool write_bulk_to_sink(Sink& sink, const char* text, size_t n) noexcept {
    if constexpr (requires(Sink& s, const char* p, size_t len) { s.write(p, len); }) {
        using R = decltype(sink.write(text, n));
        if constexpr (is_void_v<R>) {
            sink.write(text, n);
            return true;
        } else {
            return sink_result_ok(sink.write(text, n));
        }
    } else {
        for (size_t i = 0; i < n; ++i) {
            if (!write_one_to_sink(sink, text[i])) {
                return false;
            }
        }
        return true;
    }
}

} // namespace detail

template<typename Sink>
class output_context {
public:
    explicit constexpr output_context(Sink& sink) noexcept : sink_(sink) {}

    constexpr bool put(char c) noexcept {
        return write(&c, 1);
    }

    constexpr bool write(const char* text, size_t n) noexcept {
        produced_ += n;
        if (error_ != errc::ok) {
            return false;
        }
        if (n == 0) {
            return true;
        }
        if (!detail::write_bulk_to_sink(sink_, text, n)) {
            error_ = errc::output_truncated;
            return false;
        }
        return true;
    }

    constexpr bool write(StrView text) noexcept {
        return write(text.data(), text.size());
    }

    constexpr void fail(errc error) noexcept {
        if (error_ == errc::ok) {
            error_ = error;
        }
    }

    constexpr size_t produced() const noexcept { return produced_; }
    constexpr errc error() const noexcept { return error_; }
    constexpr result finish() const noexcept { return result{produced_, error_}; }

private:
    Sink& sink_;
    size_t produced_{};
    errc error_{errc::ok};
};

} // namespace libk::fmt
