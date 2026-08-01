cmake_minimum_required(VERSION 3.20)
project(dwtb VERSION 1.0.0 LANGUAGES CXX)

option(DWTB_BUILD_TESTS "Build tests" ON)
option(DWTB_WITH_KINGSROW "Build exhaustive Kingsrow comparison" OFF)

set(DWTB_KINGSROW_SOURCE "" CACHE PATH
    "Path to a local clone of eygilbert/egdb_intl")
set(DWTB_KINGSROW_DB "" CACHE PATH
    "Path containing db2.cpr1 through db5.idx1")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

find_path(ZSTD_INCLUDE_DIR NAMES zstd.h REQUIRED)
find_library(ZSTD_LIBRARY NAMES zstd libzstd REQUIRED)

add_library(dwtb_core STATIC
    src/dwtb.cpp
)
target_include_directories(dwtb_core
    PUBLIC include
    PRIVATE "${ZSTD_INCLUDE_DIR}"
)
target_link_libraries(dwtb_core
    PUBLIC Threads::Threads OpenSSL::Crypto
    PRIVATE "${ZSTD_LIBRARY}"
)

if(MSVC)
    target_compile_definitions(dwtb_core PRIVATE
        _CRT_SECURE_NO_WARNINGS NOMINMAX
    )
    target_compile_options(dwtb_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(dwtb_core PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

add_library(dwtb SHARED src/c_api.cpp)
target_link_libraries(dwtb PRIVATE dwtb_core)
target_include_directories(dwtb PUBLIC include)

add_executable(dwtb-generate src/generate_main.cpp)
target_link_libraries(dwtb-generate PRIVATE dwtb_core)

add_executable(dwtb-query src/query_main.cpp)
target_link_libraries(dwtb-query PRIVATE dwtb_core)

if(DWTB_BUILD_TESTS)
    enable_testing()

    add_executable(rules_tests tests/rules_tests.cpp)
    target_link_libraries(rules_tests PRIVATE dwtb_core)
    add_test(NAME rules_tests COMMAND rules_tests)

    add_executable(smoke_tb tests/smoke_tb.cpp)
    target_link_libraries(smoke_tb PRIVATE dwtb_core)
    add_test(NAME smoke_tb COMMAND smoke_tb)
    set_tests_properties(smoke_tb PROPERTIES RUN_SERIAL TRUE)
endif()

if(DWTB_WITH_KINGSROW)
    include(FetchContent)

    if(DWTB_KINGSROW_SOURCE)
        set(KINGSROW_ROOT "${DWTB_KINGSROW_SOURCE}")
    else()
        FetchContent_Declare(
            kingsrow_src
            GIT_REPOSITORY https://github.com/eygilbert/egdb_intl.git
            GIT_TAG master
        )
        FetchContent_GetProperties(kingsrow_src)
        if(NOT kingsrow_src_POPULATED)
            FetchContent_Populate(kingsrow_src)
        endif()
        set(KINGSROW_ROOT "${kingsrow_src_SOURCE_DIR}")
    endif()

    file(GLOB_RECURSE KINGSROW_SOURCES CONFIGURE_DEPENDS
        "${KINGSROW_ROOT}/builddb/*.cpp"
        "${KINGSROW_ROOT}/egdb/*.cpp"
        "${KINGSROW_ROOT}/engine/*.cpp"
        "${KINGSROW_ROOT}/Huffman/*.cpp"
        "${KINGSROW_ROOT}/Re-pair/*.cpp"
    )

    add_library(kingsrow_egdb STATIC ${KINGSROW_SOURCES})
    target_include_directories(kingsrow_egdb PUBLIC "${KINGSROW_ROOT}")
    target_link_libraries(kingsrow_egdb PUBLIC Threads::Threads)

    if(MSVC)
        target_compile_definitions(kingsrow_egdb PRIVATE
            _CRT_SECURE_NO_WARNINGS NOMINMAX
        )
    endif()

    add_executable(kingsrow_compare5 tests/kingsrow_compare5.cpp)
    target_link_libraries(kingsrow_compare5
        PRIVATE dwtb_core kingsrow_egdb
    )

    if(DWTB_BUILD_TESTS AND DWTB_KINGSROW_DB)
        add_test(
            NAME kingsrow_compare5
            COMMAND kingsrow_compare5
                --kingsrow "${DWTB_KINGSROW_DB}"
                --out "${CMAKE_BINARY_DIR}/international-5"
        )
        set_tests_properties(kingsrow_compare5 PROPERTIES
            RUN_SERIAL TRUE
            LABELS "exhaustive;kingsrow"
        )
    endif()
endif()
