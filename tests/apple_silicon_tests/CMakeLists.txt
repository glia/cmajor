cmake_minimum_required(VERSION 3.16..3.22)

project(
    AppleSiliconTests
    VERSION 0.1
    LANGUAGES CXX C
)

add_executable(AppleSiliconTests)

target_compile_features(AppleSiliconTests PRIVATE cxx_std_17)
target_compile_options(AppleSiliconTests PRIVATE ${CMAJ_WARNING_FLAGS})

target_sources(AppleSiliconTests
    PRIVATE
        main.cpp
        cmaj_AppleSiliconTests.h
)

target_include_directories(AppleSiliconTests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    target_link_libraries(AppleSiliconTests PRIVATE "-framework Accelerate")
endif()

enable_testing()
add_test(NAME AppleSiliconTests COMMAND AppleSiliconTests)
