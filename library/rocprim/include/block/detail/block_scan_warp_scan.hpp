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

#ifndef ROCPRIM_BLOCK_DETAIL_BLOCK_SCAN_WARP_SCAN_HPP_
#define ROCPRIM_BLOCK_DETAIL_BLOCK_SCAN_WARP_SCAN_HPP_

#include <type_traits>

// HC API
#include <hcc/hc.hpp>

#include "../../detail/config.hpp"
#include "../../detail/various.hpp"

#include "../../intrinsics.hpp"
#include "../../functional.hpp"

#include "../../warp/warp_scan.hpp"

BEGIN_ROCPRIM_NAMESPACE

namespace detail
{

template<
    class T,
    unsigned int BlockSize
>
class block_scan_warp_scan
{
    // Select warp size
    static constexpr unsigned int warp_size_ =
        detail::get_min_warp_size(BlockSize, ::rocprim::warp_size());
    // Number of warps in block
    static constexpr unsigned int warps_no_ = (BlockSize + warp_size_ - 1) / warp_size_;

    // typedef of warp_scan primitive that will be used to perform warp-level
    // inclusive/exclusive scan operations on input values.
    // warp_scan_shuffle is an implementation of warp_scan that does not need storage,
    // but requires logical warp size to be a power of two.
    using warp_scan_input_type = ::rocprim::detail::warp_scan_shuffle<T, warp_size_>;
    // typedef of warp_scan primitive that will be used to get prefix values for
    // each warp (scanned carry-outs from warps before it).
    using warp_scan_prefix_type = ::rocprim::detail::warp_scan_shuffle<T, detail::next_power_of_two(warps_no_)>;

public:
    struct storage_type
    {
        T warp_prefixes[warps_no_];
        // ---------- Shared memory optimisation ----------
        // Since warp_scan_input and warp_scan_prefix are typedef of warp_scan_shuffle,
        // we don't need to allocate any temporary memory for them.
        // If we just use warp_scan, we would need to add following union to this struct:
        // union
        // {
        //     typename warp_scan_input::storage_type wscan[warps_no_];
        //     typename warp_scan_prefix::storage_type wprefix_scan;
        // };
        // and use storage.wscan[warp_id] and storage.wprefix_scan when calling
        // warp_scan_input().inclusive_scan(..) and warp_scan_prefix().inclusive_scan(..).
    };

    template<class BinaryFunction>
    void inclusive_scan(T input,
                        T& output,
                        storage_type& storage,
                        BinaryFunction scan_op) [[hc]]
    {
        this->inclusive_scan_impl(
            ::rocprim::flat_block_thread_id(),
            input, output, storage, scan_op
        );
    }

    template<class BinaryFunction>
    void inclusive_scan(T input,
                        T& output,
                        BinaryFunction scan_op) [[hc]]
    {
        tile_static storage_type storage;
        this->inclusive_scan(input, output, storage, scan_op);
    }

    template<class BinaryFunction>
    void inclusive_scan(T input,
                        T& output,
                        T& reduction,
                        storage_type& storage,
                        BinaryFunction scan_op) [[hc]]
    {
        this->inclusive_scan(input, output, storage, scan_op);
        // Save reduction result
        reduction = storage.warp_prefixes[warps_no_ - 1];
    }

    template<class BinaryFunction>
    void inclusive_scan(T input,
                        T& output,
                        T& reduction,
                        BinaryFunction scan_op) [[hc]]
    {
        tile_static storage_type storage;
        this->inclusive_scan(input, output, reduction, storage, scan_op);
    }

    template<class PrefixCallback, class BinaryFunction>
    void inclusive_scan(T input,
                        T& output,
                        PrefixCallback& prefix_callback_op,
                        storage_type& storage,
                        BinaryFunction scan_op) [[hc]]
    {
        const auto flat_tid = ::rocprim::flat_block_thread_id();
        const auto warp_id = ::rocprim::warp_id();
        this->inclusive_scan_impl(flat_tid, input, output, storage, scan_op);
        // Include block prefix (this operation overwrites storage.warp_prefixes[warps_no_ - 1])
        T block_prefix = this->get_block_prefix(
            flat_tid, warp_id,
            storage.warp_prefixes[warps_no_ - 1], // block reduction
            prefix_callback_op, storage
        );
        output = scan_op(block_prefix, output);
    }

    template<unsigned int ItemsPerThread, class BinaryFunction>
    void inclusive_scan(T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        storage_type& storage,
                        BinaryFunction scan_op) [[hc]]
    {
        // Reduce thread items
        T thread_input = input[0];
        #pragma unroll
        for(unsigned int i = 1; i < ItemsPerThread; i++)
        {
            thread_input = scan_op(thread_input, input[i]);
        }

        // Scan of reduced values to get prefixes
        const auto flat_tid = ::rocprim::flat_block_thread_id();
        this->exclusive_scan_impl(
            flat_tid,
            thread_input, thread_input, // input, output
            storage,
            scan_op
        );

        // Include prefix (first thread does not have prefix)
        output[0] = flat_tid == 0 ? input[0] : scan_op(thread_input, input[0]);
        // Final thread-local scan
        #pragma unroll
        for(unsigned int i = 1; i < ItemsPerThread; i++)
        {
            output[i] = scan_op(output[i-1], input[i]);
        }
    }

    template<unsigned int ItemsPerThread, class BinaryFunction>
    void inclusive_scan(T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        BinaryFunction scan_op) [[hc]]
    {
        tile_static storage_type storage;
        this->inclusive_scan(input, output, storage, scan_op);
    }

    template<unsigned int ItemsPerThread, class BinaryFunction>
    void inclusive_scan(T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        T& reduction,
                        storage_type& storage,
                        BinaryFunction scan_op) [[hc]]
    {
        this->inclusive_scan(input, output, storage, scan_op);
        // Save reduction result
        reduction = storage.warp_prefixes[warps_no_ - 1];
    }

    template<unsigned int ItemsPerThread, class BinaryFunction>
    void inclusive_scan(T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        T& reduction,
                        BinaryFunction scan_op) [[hc]]
    {
        tile_static storage_type storage;
        this->inclusive_scan(input, output, reduction, storage, scan_op);
    }

    template<
        class PrefixCallback,
        unsigned int ItemsPerThread,
        class BinaryFunction
    >
    void inclusive_scan(T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        PrefixCallback& prefix_callback_op,
                        storage_type& storage,
                        BinaryFunction scan_op) [[hc]]
    {
        // Reduce thread items
        T thread_input = input[0];
        #pragma unroll
        for(unsigned int i = 1; i < ItemsPerThread; i++)
        {
            thread_input = scan_op(thread_input, input[i]);
        }

        // Scan of reduced values to get prefixes
        const auto flat_tid = ::rocprim::flat_block_thread_id();
        this->exclusive_scan_impl(
            flat_tid,
            thread_input, thread_input, // input, output
            storage,
            scan_op
        );

        // this operation overwrites storage.warp_prefixes[warps_no_ - 1]
        T block_prefix = this->get_block_prefix(
            flat_tid, ::rocprim::warp_id(),
            storage.warp_prefixes[warps_no_ - 1], // block reduction
            prefix_callback_op, storage
        );

        // Include prefix (first thread does not have prefix)
        output[0] = flat_tid == 0 ? input[0] : scan_op(thread_input, input[0]);
        // Include block prefix
        output[0] = scan_op(block_prefix, output[0]);
        // Final thread-local scan
        #pragma unroll
        for(unsigned int i = 1; i < ItemsPerThread; i++)
        {
            output[i] = scan_op(output[i-1], input[i]);
        }
    }

    template<class BinaryFunction>
    void exclusive_scan(T input,
                        T& output,
                        T init,
                        storage_type& storage,
                        BinaryFunction scan_op) [[hc]]
    {
        this->exclusive_scan_impl(
            ::rocprim::flat_block_thread_id(),
            input, output, init, storage, scan_op
        );
    }

    template<class BinaryFunction>
    void exclusive_scan(T input,
                        T& output,
                        T init,
                        BinaryFunction scan_op) [[hc]]
    {
        tile_static storage_type storage;
        this->exclusive_scan(
            input, output, init, storage, scan_op
        );
    }

    template<class BinaryFunction>
    void exclusive_scan(T input,
                        T& output,
                        T init,
                        T& reduction,
                        storage_type& storage,
                        BinaryFunction scan_op) [[hc]]
    {
        this->exclusive_scan(
            input, output, init, storage, scan_op
        );
        // Save reduction result
        reduction = storage.warp_prefixes[warps_no_ - 1];
    }

    template<class BinaryFunction>
    void exclusive_scan(T input,
                        T& output,
                        T init,
                        T& reduction,
                        BinaryFunction scan_op) [[hc]]
    {
        tile_static storage_type storage;
        this->exclusive_scan(
            input, output, init, reduction, storage, scan_op
        );
    }

    template<class PrefixCallback, class BinaryFunction>
    void exclusive_scan(T input,
                        T& output,
                        PrefixCallback& prefix_callback_op,
                        storage_type& storage,
                        BinaryFunction scan_op) [[hc]]
    {
        const auto flat_tid = ::rocprim::flat_block_thread_id();
        const auto warp_id = ::rocprim::warp_id();
        this->exclusive_scan_impl(
            flat_tid, input, output, storage, scan_op
        );
        // Include block prefix (this operation overwrites storage.warp_prefixes[warps_no_ - 1])
        T block_prefix = this->get_block_prefix(
            flat_tid, warp_id,
            storage.warp_prefixes[warps_no_ - 1], // block reduction
            prefix_callback_op, storage
        );
        output = flat_tid == 0 ? block_prefix : scan_op(block_prefix, output);
    }

    template<unsigned int ItemsPerThread, class BinaryFunction>
    void exclusive_scan(T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        T init,
                        storage_type& storage,
                        BinaryFunction scan_op) [[hc]]
    {
        // Reduce thread items
        T thread_input = input[0];
        #pragma unroll
        for(unsigned int i = 1; i < ItemsPerThread; i++)
        {
            thread_input = scan_op(thread_input, input[i]);
        }

        // Scan of reduced values to get prefixes
        const auto flat_tid = ::rocprim::flat_block_thread_id();
        this->exclusive_scan_impl(
            flat_tid,
            thread_input, thread_input, // input, output
            init,
            storage,
            scan_op
        );

        // Include init value
        T prev = input[0];
        T exclusive = flat_tid == 0 ? init : thread_input;
        output[0] = exclusive;
        #pragma unroll
        for(unsigned int i = 1; i < ItemsPerThread; i++)
        {
            exclusive = scan_op(exclusive, prev);
            prev = input[i];
            output[i] = exclusive;
        }
    }

    template<unsigned int ItemsPerThread, class BinaryFunction>
    void exclusive_scan(T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        T init,
                        BinaryFunction scan_op) [[hc]]
    {
        tile_static storage_type storage;
        this->exclusive_scan(input, output, init, storage, scan_op);
    }

    template<unsigned int ItemsPerThread, class BinaryFunction>
    void exclusive_scan(T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        T init,
                        T& reduction,
                        storage_type& storage,
                        BinaryFunction scan_op) [[hc]]
    {
        this->exclusive_scan(input, output, init, storage, scan_op);
        // Save reduction result
        reduction = storage.warp_prefixes[warps_no_ - 1];
    }

    template<unsigned int ItemsPerThread, class BinaryFunction>
    void exclusive_scan(T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        T init,
                        T& reduction,
                        BinaryFunction scan_op) [[hc]]
    {
        tile_static storage_type storage;
        this->exclusive_scan(input, output, init, reduction, storage, scan_op);
    }

    template<
        class PrefixCallback,
        unsigned int ItemsPerThread,
        class BinaryFunction
    >
    void exclusive_scan(T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        PrefixCallback& prefix_callback_op,
                        storage_type& storage,
                        BinaryFunction scan_op) [[hc]]
    {
        // Reduce thread items
        T thread_input = input[0];
        #pragma unroll
        for(unsigned int i = 1; i < ItemsPerThread; i++)
        {
            thread_input = scan_op(thread_input, input[i]);
        }

        // Scan of reduced values to get prefixes
        const auto flat_tid = ::rocprim::flat_block_thread_id();
        this->exclusive_scan_impl(
            flat_tid,
            thread_input, thread_input, // input, output
            storage,
            scan_op
        );

        // this operation overwrites storage.warp_prefixes[warps_no_ - 1]
        T block_prefix = this->get_block_prefix(
            flat_tid, ::rocprim::warp_id(),
            storage.warp_prefixes[warps_no_ - 1], // block reduction
            prefix_callback_op, storage
        );

        // Include init value and block prefix
        T prev = input[0];
        T exclusive = flat_tid == 0 ? block_prefix : scan_op(block_prefix, thread_input);
        output[0] = exclusive;
        #pragma unroll
        for(unsigned int i = 1; i < ItemsPerThread; i++)
        {
            exclusive = scan_op(exclusive, prev);
            prev = input[i];
            output[i] = exclusive;
        }
    }

private:
    template<class BinaryFunction>
    void inclusive_scan_impl(const unsigned int flat_tid,
                             T input,
                             T& output,
                             storage_type& storage,
                             BinaryFunction scan_op) [[hc]]
    {
        // Perform warp scan
        warp_scan_input_type().inclusive_scan(
            // not using shared mem, see note in storage_type
            input, output, scan_op
        );

        // i-th warp will have its prefix stored in storage.warp_prefixes[i-1]
        const auto warp_id = ::rocprim::warp_id();
        this->calculate_warp_prefixes(flat_tid, warp_id, output, storage, scan_op);

        // Use warp prefix to calculate the final scan results for every thread
        if(warp_id != 0)
        {
            auto warp_prefix = storage.warp_prefixes[warp_id - 1];
            output = scan_op(warp_prefix, output);
        }
    }

    template<class BinaryFunction>
    void exclusive_scan_impl(const unsigned int flat_tid,
                             T input,
                             T& output,
                             T init,
                             storage_type& storage,
                             BinaryFunction scan_op) [[hc]]
    {
        // Perform warp scan on input values
        warp_scan_input_type().inclusive_scan(
            // not using shared mem, see note in storage_type
            input, output, scan_op
        );

        // i-th warp will have its prefix stored in storage.warp_prefixes[i-1]
        const auto warp_id = ::rocprim::warp_id();
        const auto lane_id = ::rocprim::lane_id();
        this->calculate_warp_prefixes(flat_tid, warp_id, output, storage, scan_op);

        // Include initial value in warp prefixes, and fix warp prefixes
        // for exclusive scan (first warp prefix is init)
        auto warp_prefix = init;
        if(warp_id != 0)
        {
            warp_prefix = scan_op(init, storage.warp_prefixes[warp_id-1]);
        }

        // Use warp prefix to calculate the final scan results for every thread
        output = scan_op(warp_prefix, output); // include warp prefix in scan results
        output = warp_shuffle_up(output, 1, warp_size_); // shift to get exclusive results
        output = lane_id == 0 ? warp_prefix : output;
    }

    // Exclusive scan with unknown initial value
    template<class BinaryFunction>
    void exclusive_scan_impl(const unsigned int flat_tid,
                             T input,
                             T& output,
                             storage_type& storage,
                             BinaryFunction scan_op) [[hc]]
    {
        // Perform warp scan on input values
        warp_scan_input_type().inclusive_scan(
            // not using shared mem, see note in storage_type
            input, output, scan_op
        );

        // i-th warp will have its prefix stored in storage.warp_prefixes[i-1]
        const auto warp_id = ::rocprim::warp_id();
        const auto lane_id = ::rocprim::lane_id();
        this->calculate_warp_prefixes(flat_tid, warp_id, output, storage, scan_op);

        // Use warp prefix to calculate the final scan results for every thread
        T warp_prefix;
        if(warp_id != 0)
        {
            warp_prefix = storage.warp_prefixes[warp_id - 1];
            output = scan_op(warp_prefix, output);
        }
        output = warp_shuffle_up(output, 1, warp_size_); // shift to get exclusive results
        output = lane_id == 0 ? warp_prefix : output;
    }

    // i-th warp will have its prefix stored in storage.warp_prefixes[i-1]
    template<class BinaryFunction>
    void calculate_warp_prefixes(const unsigned int flat_tid,
                                 const unsigned int warp_id,
                                 T inclusive_input,
                                 storage_type& storage,
                                 BinaryFunction scan_op) [[hc]]
    {
        // Save the warp reduction result, that is the scan result
        // for last element in each warp
        if(flat_tid == ::rocprim::min((warp_id+1) * warp_size_, BlockSize) - 1)
        {
            storage.warp_prefixes[warp_id] = inclusive_input;
        }
        ::rocprim::syncthreads();

        // Scan the warp reduction results and store in storage.warp_prefixes
        if(flat_tid < warps_no_)
        {
            auto warp_prefix = storage.warp_prefixes[flat_tid];
            warp_scan_prefix_type().inclusive_scan(
                // not using shared mem, see note in storage_type
                warp_prefix, warp_prefix, scan_op
            );
            storage.warp_prefixes[flat_tid] = warp_prefix;
        }
        ::rocprim::syncthreads();
    }

    // THIS OVERWRITES storage.warp_prefixes[warps_no_ - 1]
    template<class PrefixCallback>
    T get_block_prefix(const unsigned int flat_tid,
                       const unsigned int warp_id,
                       const T reduction,
                       PrefixCallback& prefix_callback_op,
                       storage_type& storage) [[hc]]
    {
        if(warp_id == 0)
        {
            T block_prefix = prefix_callback_op(reduction);
            if(flat_tid == 0)
            {
                // Reuse storage.warp_prefixes[warps_no_ - 1] to store block prefix
                storage.warp_prefixes[warps_no_ - 1] = block_prefix;
            }
        }
        ::rocprim::syncthreads();
        return storage.warp_prefixes[warps_no_ - 1];
    }
};

} // end namespace detail

END_ROCPRIM_NAMESPACE

#endif // ROCPRIM_BLOCK_DETAIL_BLOCK_SCAN_WARP_SCAN_HPP_
