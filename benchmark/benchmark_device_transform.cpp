// MIT License
//
// Copyright (c) 2017-2024 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "benchmark_utils.hpp"
// CmdParser
#include "cmdparser.hpp"

// Google Benchmark
#include <benchmark/benchmark.h>

// HIP API
#include <hip/hip_runtime.h>

// rocPRIM
#include <rocprim/device/device_transform.hpp>

#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <cstdio>
#include <cstdlib>

#ifndef DEFAULT_N
const size_t DEFAULT_N = 1024 * 1024 * 128;
#endif

const unsigned int batch_size = 10;
const unsigned int warmup_size = 5;

template<class T>
struct transform
{
    __device__ __host__
    constexpr T operator()(const T& a) const
    {
        return a + T(5);
    }
};

template<
    class T,
    class BinaryFunction
>
void run_benchmark(benchmark::State& state,
                   size_t size,
                   const hipStream_t stream,
                   BinaryFunction transform_op)
{
    std::vector<T> input = get_random_data<T>(size, T(0), T(1000));

    T * d_input;
    T * d_output;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&d_input), size * sizeof(T)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&d_output), size * sizeof(T)));
    HIP_CHECK(
        hipMemcpy(
            d_input, input.data(),
            size * sizeof(T),
            hipMemcpyHostToDevice
        )
    );
    HIP_CHECK(hipDeviceSynchronize());

    // Warm-up
    for(size_t i = 0; i < warmup_size; i++)
    {
        HIP_CHECK(
            rocprim::transform(
                d_input, d_output, size,
                transform_op, stream
            )
        );
    }
    HIP_CHECK(hipDeviceSynchronize());

    // HIP events creation
    hipEvent_t start, stop;
    HIP_CHECK(hipEventCreate(&start));
    HIP_CHECK(hipEventCreate(&stop));

    for(auto _ : state)
    {
        // Record start event
        HIP_CHECK(hipEventRecord(start, stream));

        for(size_t i = 0; i < batch_size; i++)
        {
            HIP_CHECK(
                rocprim::transform(
                    d_input, d_output, size,
                    transform_op, stream
                )
            );
        }

        // Record stop event and wait until it completes
        HIP_CHECK(hipEventRecord(stop, stream));
        HIP_CHECK(hipEventSynchronize(stop));

        float elapsed_mseconds;
        HIP_CHECK(hipEventElapsedTime(&elapsed_mseconds, start, stop));
        state.SetIterationTime(elapsed_mseconds / 1000);
    }

    // Destroy HIP events
    HIP_CHECK(hipEventDestroy(start));
    HIP_CHECK(hipEventDestroy(stop));

    state.SetBytesProcessed(state.iterations() * batch_size * size * sizeof(T));
    state.SetItemsProcessed(state.iterations() * batch_size * size);

    HIP_CHECK(hipFree(d_input));
    HIP_CHECK(hipFree(d_output));
}

#define CREATE_BENCHMARK(T, TRANSFORM_OP)                                                \
    benchmark::RegisterBenchmark(                                                        \
        bench_naming::format_name("{lvl:device,algo:transform,key_type:" #T              \
                                  ",transform_op:" #TRANSFORM_OP ",cfg:default_config}") \
            .c_str(),                                                                    \
        run_benchmark<T, TRANSFORM_OP>,                                                  \
        size,                                                                            \
        stream,                                                                          \
        TRANSFORM_OP())

int main(int argc, char *argv[])
{
    cli::Parser parser(argc, argv);
    parser.set_optional<size_t>("size", "size", DEFAULT_N, "number of values");
    parser.set_optional<int>("trials", "trials", -1, "number of iterations");
    parser.set_optional<std::string>("name_format",
                                     "name_format",
                                     "human",
                                     "either: json,human,txt");
    parser.run_and_exit_if_error();

    // Parse argv
    benchmark::Initialize(&argc, argv);
    const size_t size = parser.get<size_t>("size");
    const int trials = parser.get<int>("trials");
    bench_naming::set_format(parser.get<std::string>("name_format"));

    // HIP
    hipStream_t stream = 0; // default

    // Benchmark info
    add_common_benchmark_info();
    benchmark::AddCustomContext("size", std::to_string(size));

    using custom_float2 = custom_type<float, float>;
    using custom_double2 = custom_type<double, double>;

    // Add benchmarks
    std::vector<benchmark::internal::Benchmark*> benchmarks =
    {
        CREATE_BENCHMARK(int, transform<int>),
        CREATE_BENCHMARK(long long, transform<long long>),

        CREATE_BENCHMARK(int8_t, transform<int8_t>),
        CREATE_BENCHMARK(uint8_t, transform<uint8_t>),
        CREATE_BENCHMARK(rocprim::half, transform<rocprim::half>),

        CREATE_BENCHMARK(float, transform<float>),
        CREATE_BENCHMARK(double, transform<double>),

        CREATE_BENCHMARK(custom_float2, transform<custom_float2>),
        CREATE_BENCHMARK(custom_double2, transform<custom_double2>),
    };

    // Use manual timing
    for(auto& b : benchmarks)
    {
        b->UseManualTime();
        b->Unit(benchmark::kMillisecond);
    }

    // Force number of iterations
    if(trials > 0)
    {
        for(auto& b : benchmarks)
        {
            b->Iterations(trials);
        }
    }

    // Run benchmarks
    benchmark::RunSpecifiedBenchmarks();

    return 0;
}
