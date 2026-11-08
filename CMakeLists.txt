cmake_minimum_required(VERSION 3.16)
project(cpp_limit_order_book VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(order_book src/order_book.cpp)
target_include_directories(order_book PUBLIC include)
target_compile_options(order_book PRIVATE -Wall -Wextra -Wpedantic)

add_executable(lob_demo src/main.cpp)
target_link_libraries(lob_demo PRIVATE order_book)

add_executable(lob_tests tests/order_book_tests.cpp)
target_link_libraries(lob_tests PRIVATE order_book)

enable_testing()
add_test(NAME order_book_tests COMMAND lob_tests)
