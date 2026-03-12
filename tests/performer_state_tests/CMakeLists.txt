cmake_minimum_required(VERSION 3.16..3.22)

project(
    PerformerStateTests
    VERSION 0.1
    LANGUAGES CXX C
)

add_executable(PerformerStateTests)

target_compile_features(PerformerStateTests PRIVATE cxx_std_17)
target_compile_options(PerformerStateTests PRIVATE ${CMAJ_WARNING_FLAGS})

target_sources(PerformerStateTests
    PRIVATE
        main.cpp
        cmaj_PerformerStateTests.h
)

target_include_directories(PerformerStateTests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

enable_testing()
add_test(NAME PerformerStateTests COMMAND PerformerStateTests)
