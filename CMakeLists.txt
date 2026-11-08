cmake_minimum_required(VERSION 3.16)
project(cpp_limit_order_book VERSION 1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOB_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(lob_warnings INTERFACE)
target_compile_options(lob_warnings INTERFACE -Wall -Wextra -Wpedantic)
if(LOB_SANITIZE)
    target_compile_options(lob_warnings INTERFACE
        -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(lob_warnings INTERFACE -fsanitize=address,undefined)
endif()

add_library(order_book src/order_book.cpp)
target_include_directories(order_book PUBLIC include)
target_link_libraries(order_book PRIVATE lob_warnings)

add_executable(lob_demo src/main.cpp)
target_link_libraries(lob_demo PRIVATE order_book lob_warnings)

add_executable(lob_tests tests/order_book_tests.cpp)
target_include_directories(lob_tests PRIVATE tests)
target_link_libraries(lob_tests PRIVATE order_book lob_warnings)

enable_testing()
add_test(NAME order_book_tests COMMAND lob_tests)
add_test(NAME sample_replay COMMAND lob_demo --replay ${CMAKE_CURRENT_SOURCE_DIR}/data/sample_feed.csv)
