#pragma once

#include <servers/deploy/detail/image.hpp>
#include <servers/deploy/detail/space.hpp>
#include <servers/deploy/format.hpp>
#include <libk/noncopyable.hpp>
#include <servers/runtime/service.hpp>

namespace sys::process {

class ViewDescriptor final {
    friend class Image;
    cap::OwnedCap memory_;
    cap_t cspace_{};
public:
    auto open(cap_t pool, cap_t cspace) noexcept -> status_t {
        cspace_ = cspace;
        const auto descriptor = memory_create(pool, 4096, VM_READ | VM_WRITE);
        if (descriptor.status != STATUS_OK) return descriptor.status;
        memory_ = cap::OwnedCap{{descriptor.value, 0}};
        return STATUS_OK;
    }
};

// The package mapping outlives construction and supplies only complete file
// pages. ImageMaterializer owns anonymous boundary/BSS pages in the child pool.
class Image final : private libk::noncopyable_nonmovable {
    ViewDescriptor* descriptor_{};
    cap_t package_{};
    uintptr_t package_address_{};
    size_t package_size_{};

    auto create(const boot::Segment& segment, word_t& first) noexcept -> SysResult {
        first = 0;
        const auto size = deploy::Window::round_size(segment.memory_size);
        const auto address = reinterpret_cast<uintptr_t>(segment.file);
        if (size == 0 || segment.file_size != size || address < package_address_
            || address - package_address_ > package_size_
            || size > package_size_ - (address - package_address_))
            return {.status = STATUS_BAD_ARGS};
        const auto offset = address - package_address_;
        if (offset % 4096 != 0) return {.status = STATUS_BAD_ARGS};
        first = offset / 4096;
        const auto source_access = (segment.access & VM_WRITE) != 0
            ? VM_READ : segment.access;
        const CapView view{
            .version = CAP_ATTENUATION_VERSION_CURRENT, .kind = OBJECT_KIND_MEMORY,
            .size = CAP_ATTENUATION_SIZE, .rights = RIGHT_MAP,
            .words = {first, size / 4096, source_access}};
        auto& wire = *reinterpret_cast<uint8_t (*)[CAP_ATTENUATION_SIZE]>(service::IpcAddress);
        sys::cap::encode(view, wire);
        const auto written = memory_write(descriptor_->memory_.selector(), 0, 0, sizeof(wire));
        return written.status == STATUS_OK
            ? cap_typed_delegate(package_, descriptor_->cspace_, descriptor_->memory_.selector()) : written;
    }

public:
    auto source(ViewDescriptor& descriptor, cap_t package, uintptr_t address, size_t size) noexcept -> deploy::ImageSource {
        descriptor_ = &descriptor;
        package_ = package;
        package_address_ = address;
        package_size_ = size;
        return {this, [](void* context, const boot::Segment& segment,
                         word_t& first) noexcept {
            return static_cast<Image*>(context)->create(segment, first);
        }};
    }
    void close() noexcept {
        descriptor_ = nullptr;
        package_ = 0;
        package_address_ = 0;
        package_size_ = 0;
    }
};

} // namespace sys::process
