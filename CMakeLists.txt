cmake_minimum_required(VERSION 3.20)
project(draughts_wld LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(wld STATIC
    src/wld.cpp
)

target_include_directories(wld PUBLIC include)
target_link_libraries(wld PUBLIC Threads::Threads OpenSSL::Crypto)

if(MSVC)
    target_compile_options(wld PRIVATE /W4 /permissive-)
else()
    target_compile_options(wld PRIVATE
        -Wall -Wextra -Wpedantic
        -O3 -march=native
    )
endif()

add_executable(wldgen src/main.cpp)
target_link_libraries(wldgen PRIVATE wld)

enable_testing()

add_executable(wld_tests tests/tests.cpp)
target_link_libraries(wld_tests PRIVATE wld)
add_test(NAME wld_tests COMMAND wld_tests)
