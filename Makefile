CXX := clang++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS := -Iinclude
SOURCES := src/main.cpp src/server.cpp
HEADERS := include/lb/server.hpp include/lb/buffer.hpp include/lb/backends.hpp
PYTHON ?= python3
.PHONY: all test sanitize demo benchmark
all: build/tcp-lb
build/tcp-lb: $(SOURCES) $(HEADERS)
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) -o $@
build/unit-tests: tests/unit.cpp src/server.cpp $(HEADERS)
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/unit.cpp src/server.cpp -o $@
test: build/tcp-lb build/unit-tests
	./build/unit-tests
	$(PYTHON) tests/integration.py ./build/tcp-lb
sanitize:
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit.cpp src/server.cpp -o build/unit-sanitize
	./build/unit-sanitize
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer $(SOURCES) -o build/tcp-lb-sanitize
	$(PYTHON) tests/integration.py ./build/tcp-lb-sanitize
demo: build/tcp-lb
	$(PYTHON) scripts/demo.py
benchmark: build/tcp-lb
	$(PYTHON) scripts/benchmark.py
