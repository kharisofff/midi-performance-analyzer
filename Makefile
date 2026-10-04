CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
INCLUDES := -Iinclude

.PHONY: all test clean

all: midi_analyzer

midi_analyzer: src/main.cpp src/midi.cpp include/midi.hpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) src/main.cpp src/midi.cpp -o $@

# Tests are built with AddressSanitizer and UndefinedBehaviorSanitizer so that
# out-of-bounds reads and integer overflows in the parser fail loudly.
run_tests: tests/test_midi.cpp src/midi.cpp include/midi.hpp
	$(CXX) -std=c++17 -g -O1 -Wall -Wextra -Wpedantic -fsanitize=address,undefined $(INCLUDES) tests/test_midi.cpp src/midi.cpp -o $@

test: run_tests
	./run_tests

clean:
	rm -f midi_analyzer run_tests
