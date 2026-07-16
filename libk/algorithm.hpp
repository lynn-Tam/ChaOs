#pragma once

#include <libk/utility.hpp>

namespace libk {

template<typename Iterator, typename Compare>
constexpr void insertion_sort(Iterator first, Iterator last, Compare before) {
    if (first == last) {
        return;
    }

    for (Iterator current = first + 1; current != last; ++current) {
        auto value = libk::move(*current);
        Iterator position = current;
        while (position != first && before(value, *(position - 1))) {
            *position = libk::move(*(position - 1));
            --position;
        }
        *position = libk::move(value);
    }
}

template<typename Range, typename Compare>
constexpr void insertion_sort(Range& range, Compare before) {
    libk::insertion_sort(range.begin(), range.end(), before);
}

} // namespace libk
