#pragma once

#include <mm/vspace.hpp>

namespace kernel::mm {

[[nodiscard]] inline auto node_error(NodePoolError error) noexcept
    -> VSpaceError {
    switch (error) {
    case NodePoolError::OutOfMemory:
        return VSpaceError::OutOfMemory;
    case NodePoolError::QuotaExceeded:
        return VSpaceError::QuotaExceeded;
    case NodePoolError::GenerationExhausted:
        return VSpaceError::GenerationExhausted;
    case NodePoolError::ResourceExhausted:
        return VSpaceError::ResourceExhausted;
    }
    return VSpaceError::OutOfMemory;
}

[[nodiscard]] inline auto memory_error(MemoryError error) noexcept
    -> VSpaceError {
    switch (error) {
    case MemoryError::OutOfMemory:
    case MemoryError::ResourceExhausted:
        return VSpaceError::OutOfMemory;
    case MemoryError::GenerationExhausted:
        return VSpaceError::GenerationExhausted;
    case MemoryError::Busy:
        return VSpaceError::Busy;
    case MemoryError::BackingFailed:
    case MemoryError::NotBacked:
        return VSpaceError::BackingFailed;
    case MemoryError::InvalidMemoryType:
        return VSpaceError::UnsupportedMemoryType;
    case MemoryError::InvalidAccess:
        return VSpaceError::InvalidAccess;
    case MemoryError::InvalidSize:
    case MemoryError::InvalidRange:
        return VSpaceError::InvalidRange;
    case MemoryError::InvalidState:
    case MemoryError::AttachmentState:
    case MemoryError::OwnershipMismatch:
        return VSpaceError::InvalidState;
    }
    return VSpaceError::InvalidState;
}

} // namespace kernel::mm
