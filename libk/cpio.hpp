#pragma once
#include <span>
#include <string_view>
#include <optional>
#include <cstddef>
#include <cstdint>

namespace libk {
struct cpio_file { std::string_view name; std::span<const std::byte> data; uint32_t mode; };
class cpio_view {
    std::span<const std::byte> bytes_;
    static auto field(const std::byte* p) noexcept -> std::optional<uint32_t> {
        uint32_t n{};
        for (size_t i=0; i<8; ++i) {
            const auto c=static_cast<unsigned char>(p[i]);
            const unsigned digit=c>='0' && c<='9' ? c-'0' : c>='a' && c<='f' ? c-'a'+10 : c>='A' && c<='F' ? c-'A'+10 : 16;
            if (digit==16) return {};
            n=(n<<4)|digit;
        }
        return n;
    }
public:
    explicit operator bool() const noexcept { return !bytes_.empty(); }
    auto next(size_t& offset) const noexcept -> std::optional<cpio_file> {
        if (offset>bytes_.size() || bytes_.size()-offset<110) return {};
        const auto* p=bytes_.data()+offset;
        if (std::string_view{reinterpret_cast<const char*>(p),6}!="070701") return {};
        const auto mode=field(p+14), size=field(p+54), name=field(p+94);
        if (!mode || !size || !name || !*name || *name>bytes_.size()-offset-110) return {};
        const auto names=offset+110;
        if (bytes_[names+*name-1]!=std::byte{}) return {};
        const std::string_view label{reinterpret_cast<const char*>(bytes_.data()+names),*name-1};
        if (label.empty() || label.find('\0')!=label.npos) return {};
        const auto data=(names+*name+3)&~size_t{3};
        if (data>bytes_.size() || *size>bytes_.size()-data) return {};
        offset=(data+*size+3)&~size_t{3};
        return cpio_file{label,bytes_.subspan(data,*size),*mode};
    }
    auto find(std::string_view name) const noexcept -> std::optional<cpio_file> {
        size_t offset{};
        while (auto f=next(offset)) {
            if (f->name=="TRAILER!!!") break;
            if (f->name==name) return f;
        }
        return {};
    }
    static auto parse(std::span<const std::byte> bytes) noexcept -> cpio_view {
        cpio_view v; v.bytes_=bytes;
        size_t offset{};
        while (auto f=v.next(offset)) {
            if (f->name=="TRAILER!!!") return f->data.empty() && offset<=bytes.size() ? v : cpio_view{};
        }
        return {};
    }
};
}
