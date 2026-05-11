add_executable(basic_operations_benchmark
    basic_operations_benchmark.cpp
)

target_link_libraries(basic_operations_benchmark
    PRIVATE
        ashiato::ashiato
        benchmark::benchmark_main
)

target_compile_features(basic_operations_benchmark PRIVATE cxx_std_17)
