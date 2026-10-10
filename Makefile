CXX ?= clang++
AR ?= ar
EMXX ?= em++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -pthread
CORE_ROOT := $(abspath .)
include core.mk
CORE_OBJECTS := $(patsubst $(CORE_ROOT)/src/%.cpp,build/%.o,$(CORE_SOURCES))
TEST_FLAGS := $(CXXFLAGS) $(CORE_INCLUDE_FLAGS) -I$(CORE_ROOT)/tests
WASM_FLAGS := -std=c++20 -O2 -Wall -Wextra -pthread -fno-exceptions $(CORE_INCLUDE_FLAGS)

.PHONY: all build test ecs-test systems-test runtime-test hierarchy-test jobs-test xenium-portability-test wasm-test clean format format-check boundary-check deps deps-check
all: build test

deps:
	python3 tools/dependencies.py --manifest dependencies/lock.json --repository .

deps-check:
	python3 tools/dependencies.py --manifest dependencies/lock.json --repository . --check

build: deps-check build/libriviera-engine-core.a
build/libriviera-engine-core.a: $(CORE_OBJECTS) $(CORE_HEADERS) $(CORE_XENIUM_HEADERS) Makefile core.mk | deps-check
	mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $(CORE_OBJECTS)
build/%.o: $(CORE_ROOT)/src/%.cpp $(CORE_HEADERS) $(CORE_XENIUM_HEADERS) Makefile core.mk | deps-check
	mkdir -p $(dir $@)
	$(CXX) $(TEST_FLAGS) -c $< -o $@

build/ecs-test: tests/ecs.cpp $(CORE_HEADERS) $(CORE_XENIUM_HEADERS) Makefile core.mk | deps-check
	mkdir -p build
	$(CXX) $(TEST_FLAGS) $< -o $@
build/ecs-concurrency-test: tests/ecs_concurrency.cpp $(CORE_HEADERS) $(CORE_XENIUM_HEADERS) Makefile core.mk | deps-check
	mkdir -p build
	$(CXX) $(TEST_FLAGS) $< -o $@
ecs-test: build/ecs-test build/ecs-concurrency-test
	./build/ecs-test
	./build/ecs-concurrency-test

build/systems-test: tests/systems.cpp $(CORE_SOURCES) $(CORE_HEADERS) $(CORE_XENIUM_HEADERS) Makefile core.mk | deps-check
	mkdir -p build
	$(CXX) $(TEST_FLAGS) $< $(CORE_SOURCES) -o $@
systems-test: build/systems-test
	./build/systems-test

build/runtime-test: tests/runtime.cpp $(CORE_HEADERS) Makefile core.mk | deps-check
	mkdir -p build
	$(CXX) $(TEST_FLAGS) $< -o $@
runtime-test: build/runtime-test
	./build/runtime-test

build/hierarchy-test: tests/hierarchy.cpp $(CORE_HEADERS) $(CORE_XENIUM_HEADERS) Makefile core.mk | deps-check
	mkdir -p build
	$(CXX) $(TEST_FLAGS) $< -o $@
hierarchy-test: build/hierarchy-test
	./build/hierarchy-test

build/jobs-test: tests/jobs.cpp $(CORE_SOURCES) $(CORE_HEADERS) $(CORE_XENIUM_HEADERS) Makefile core.mk | deps-check
	mkdir -p build
	$(CXX) $(TEST_FLAGS) $(filter %.cpp,$^) -o $@
jobs-test: build/jobs-test
	./build/jobs-test

build/xenium-portability-native-test: tests/xenium_portability.cpp $(CORE_XENIUM_HEADERS) Makefile core.mk | deps-check
	mkdir -p build
	$(CXX) $(TEST_FLAGS) $< -o $@
xenium-portability-test: build/xenium-portability-native-test
	./build/xenium-portability-native-test

build/wasm-ecs-test.js: tests/ecs.cpp $(CORE_HEADERS) $(CORE_XENIUM_HEADERS) Makefile core.mk | deps-check
	mkdir -p build
	$(EMXX) $(WASM_FLAGS) $< -o $@ -pthread -sPTHREAD_POOL_SIZE=4 -sENVIRONMENT=node -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1
build/wasm-ecs-concurrency-test.js: tests/ecs_concurrency.cpp $(CORE_HEADERS) $(CORE_XENIUM_HEADERS) Makefile core.mk | deps-check
	mkdir -p build
	$(EMXX) $(WASM_FLAGS) $< -o $@ -pthread -sPTHREAD_POOL_SIZE=4 -sENVIRONMENT=node -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1
build/wasm-xenium-portability-test.js: tests/xenium_portability.cpp $(CORE_XENIUM_HEADERS) Makefile core.mk | deps-check
	mkdir -p build
	$(EMXX) $(WASM_FLAGS) $< -o $@ -pthread -sPTHREAD_POOL_SIZE=4 -sENVIRONMENT=node -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1
build/wasm-jobs-test.js: tests/jobs.cpp $(CORE_SOURCES) $(CORE_HEADERS) $(CORE_XENIUM_HEADERS) Makefile core.mk | deps-check
	mkdir -p build
	$(EMXX) $(WASM_FLAGS) $(filter %.cpp,$^) -o $@ -pthread -sPTHREAD_POOL_SIZE=4 -sENVIRONMENT=node -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1
build/wasm-runtime-test.js: tests/runtime.cpp $(CORE_HEADERS) $(CORE_XENIUM_HEADERS) Makefile core.mk | deps-check
	mkdir -p build
	$(EMXX) $(WASM_FLAGS) $< -o $@ -pthread -sPTHREAD_POOL_SIZE=4 -sENVIRONMENT=node -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1
build/wasm-systems-test.js: tests/systems.cpp $(CORE_SOURCES) $(CORE_HEADERS) $(CORE_XENIUM_HEADERS) Makefile core.mk | deps-check
	mkdir -p build
	$(EMXX) $(WASM_FLAGS) $< $(CORE_SOURCES) -o $@ -pthread -sPTHREAD_POOL_SIZE=4 -sENVIRONMENT=node -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1
wasm-test: build/wasm-ecs-test.js build/wasm-ecs-concurrency-test.js build/wasm-xenium-portability-test.js build/wasm-jobs-test.js build/wasm-runtime-test.js build/wasm-systems-test.js
	node build/wasm-ecs-test.js
	node build/wasm-ecs-concurrency-test.js
	node build/wasm-xenium-portability-test.js
	node build/wasm-jobs-test.js
	node build/wasm-runtime-test.js
	node build/wasm-systems-test.js

test: deps-check ecs-test systems-test runtime-test hierarchy-test jobs-test xenium-portability-test boundary-check

boundary-check: deps-check
	python3 tools/check_boundary.py

-include tools/format.mk

clean:
	rm -rf build *.o
