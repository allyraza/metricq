cmake_minimum_required(VERSION 3.8)

project(metricq-source-stress-test VERSION 1.0)

add_executable(metricq-source-stress-test src/main.cpp src/stress_test_source.cpp)
target_compile_features(metricq-source-stress-test PRIVATE cxx_std_17)
target_compile_options(metricq-source-stress-test PRIVATE -Wall -Wextra -pedantic)

target_link_libraries(metricq-source-stress-test
    PRIVATE
        metricq::source
        metricq::logger-nitro
        Nitro::broken-options
)

install(TARGETS metricq-source-stress-test RUNTIME DESTINATION bin)
