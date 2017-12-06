// Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef ROCPRIM_WARP_WARP_SCAN_HPP_
#define ROCPRIM_WARP_WARP_SCAN_HPP_

#include <type_traits>

// HC API
#include <hcc/hc.hpp>

#include "../detail/config.hpp"
#include "../detail/various.hpp"

#include "../intrinsics.hpp"
#include "../functional.hpp"
#include "../types.hpp"

#include "detail/warp_scan_shuffle.hpp"

BEGIN_ROCPRIM_NAMESPACE

namespace detail
{

template<
    class T,
    unsigned int WarpSize
>
class warp_scan_shared_mem
{
public:
    static_assert(
        detail::is_power_of_two(WarpSize),
        "warp_scan is not implemented for WarpSizes that are not power of two."
    );

    typedef detail::empty_type storage;
};

// Select warp_scan implementation based WarpSize
template<class T, unsigned int WarpSize>
struct select_warp_scan_impl
{
    typedef typename std::conditional<
        // can we use shuffle-based implementation?
        detail::is_warpsize_shuffleable<WarpSize>::value,
        detail::warp_scan_shuffle<T, WarpSize>, // yes
        detail::warp_scan_shared_mem<T, WarpSize> // no
    >::type type;
};

} // end namespace detail

/// \brief Parallel scan primitive for warp.
template<
    class T,
    unsigned int WarpSize = warp_size()
>
class warp_scan : detail::select_warp_scan_impl<T, WarpSize>::type
{
    using base_type = typename detail::select_warp_scan_impl<T, WarpSize>::type;

public:
    using storage_type = typename base_type::storage_type;
    using scan_reduction_result_type = scan_reduction_type<T>;

    template<class BinaryFunction = ::rocprim::plus<T>>
    T inclusive_scan(T thread_value,
                     storage_type& storage,
                     BinaryFunction scan_op = BinaryFunction()) [[hc]]
    {
        return base_type::inclusive_scan(
            thread_value, storage, scan_op
        );
    }

    template<class BinaryFunction = ::rocprim::plus<T>>
    T inclusive_scan(T thread_value, BinaryFunction scan_op = BinaryFunction()) [[hc]]
    {
        storage_type storage;
        return base_type::inclusive_scan(thread_value, storage, scan_op);
    }

    template<class BinaryFunction = ::rocprim::plus<T>>
    scan_reduction_result_type
    inclusive_scan_reduce(T thread_value,
                          storage_type& storage,
                          BinaryFunction scan_op = BinaryFunction()) [[hc]]
    {
        return base_type::inclusive_scan_reduce(
            thread_value, storage, scan_op
        );
    }

    template<class BinaryFunction = ::rocprim::plus<T>>
    scan_reduction_result_type
    inclusive_scan_reduce(T thread_value,
                          BinaryFunction scan_op = BinaryFunction()) [[hc]]
    {
        storage_type storage;
        return base_type::inclusive_scan_reduce(
            thread_value, storage, scan_op
        );
    }
};

END_ROCPRIM_NAMESPACE

#endif // ROCPRIM_WARP_WARP_SCAN_HPP_
