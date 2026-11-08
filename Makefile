CXX ?= g++
CXXFLAGS ?= -std=c++20 -O3 -Wall -Wextra -Wpedantic -Iinclude
DEBUG_FLAGS = -std=c++20 -O0 -g3 -Wall -Wextra -Wpedantic -Iinclude
SANITIZE_FLAGS = -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -Iinclude \
                 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

BUILD_DIR = build
ENGINE_OBJ = $(BUILD_DIR)/order_book.o

.PHONY: all demo replay benchmark test debug sanitize clean

all: $(BUILD_DIR)/lob_demo $(BUILD_DIR)/lob_tests

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(ENGINE_OBJ): src/order_book.cpp include/order_book.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/lob_demo: src/main.cpp $(ENGINE_OBJ) include/order_book.hpp
	$(CXX) $(CXXFLAGS) src/main.cpp $(ENGINE_OBJ) -o $@

$(BUILD_DIR)/lob_tests: tests/order_book_tests.cpp tests/reference_book.hpp $(ENGINE_OBJ) include/order_book.hpp
	$(CXX) $(CXXFLAGS) -Itests tests/order_book_tests.cpp $(ENGINE_OBJ) -o $@

demo: $(BUILD_DIR)/lob_demo
	./$(BUILD_DIR)/lob_demo --demo

replay: $(BUILD_DIR)/lob_demo
	./$(BUILD_DIR)/lob_demo --replay data/sample_feed.csv

benchmark: $(BUILD_DIR)/lob_demo
	./$(BUILD_DIR)/lob_demo --benchmark 1000000

test: $(BUILD_DIR)/lob_tests
	./$(BUILD_DIR)/lob_tests

debug: CXXFLAGS = $(DEBUG_FLAGS)
debug: clean all

sanitize: CXXFLAGS = $(SANITIZE_FLAGS)
sanitize: clean all
	./$(BUILD_DIR)/lob_tests
	./$(BUILD_DIR)/lob_demo --replay data/sample_feed.csv > /dev/null

clean:
	rm -rf $(BUILD_DIR)
