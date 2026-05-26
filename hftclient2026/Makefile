# Standalone Makefile — builds the client without cmake/nlohmann_json.
# Usage:  make           (release build)
#         make debug     (debug symbols, no opt)
#         make clean
#
# Output: ./hftclient2026

CXX      ?= clang++
CXXSTD    = -std=c++20
WARN      = -Wall -Wextra
OPT       = -O3 -DNDEBUG -fno-rtti -fno-exceptions -fvisibility=hidden
ARCH      = $(shell $(CXX) -march=native -E - </dev/null >/dev/null 2>&1 && echo -march=native || echo -mcpu=apple-m1)
LTO       = -flto

CXXFLAGS  = $(CXXSTD) $(WARN) $(OPT) $(ARCH) $(LTO)
LDFLAGS   = $(LTO)

TARGET    = hftclient2026
SRC       = main.cpp

.PHONY: all debug clean run-local

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

debug: CXXFLAGS = $(CXXSTD) $(WARN) -O0 -g
debug: LDFLAGS  =
debug: $(TARGET)

run-local: $(TARGET)
	./$(TARGET) 127.0.0.1 12345 TeamLocal

clean:
	rm -f $(TARGET)
