#include <core/kernel_image.hpp>

#ifndef MYOS_BUILD_ID
#define MYOS_BUILD_ID "unknown"
#endif

namespace kernel::image {

const char build_id[] = MYOS_BUILD_ID;

} // namespace kernel::image
