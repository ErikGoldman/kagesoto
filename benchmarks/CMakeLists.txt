add_executable(ecs_benchmark_entities
    entities_benchmark.cpp
)

add_executable(ecs_benchmark_basic
    basic_systems_benchmark.cpp
)

add_executable(ecs_benchmark_extended
    extended_benchmark.cpp
)

add_executable(ecs_benchmark_stress
    stress_benchmark.cpp
)

add_executable(ecs_benchmark_trace
    trace_benchmark.cpp
)

foreach(target IN ITEMS ecs_benchmark_entities ecs_benchmark_basic ecs_benchmark_extended ecs_benchmark_stress ecs_benchmark_trace)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    target_link_libraries(${target}
        PRIVATE
            ecs::ecs
            benchmark::benchmark
    )

    target_compile_definitions(${target}
        PRIVATE
            ECS_BENCHMARK_VERSION="${PROJECT_VERSION}"
    )
endforeach()
