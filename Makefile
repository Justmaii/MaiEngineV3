# MaiEngine v3
#   make            -> optimize derleme (bu makineye göre: -march/-mcpu=native)
#   make debug      -> assert + sanitizer'lı derleme (maiengine-debug)
#   make test       -> perft takımı + tutarlılık kontrolü
#   make ARCH_FLAGS=... ile mimari bayrağını değiştirebilirsin

CXX ?= c++
EXE = maiengine
SRCS = $(wildcard src/*.cpp)
HDRS = $(wildcard src/*.h)

UNAME_M := $(shell uname -m)
ifeq ($(filter $(UNAME_M),arm64 aarch64),)
  ARCH_FLAGS ?= -march=native
else
  ARCH_FLAGS ?= -mcpu=native
endif

CXXFLAGS_COMMON = -std=c++20 -Wall -Wextra -Wshadow -pedantic
CXXFLAGS_RELEASE = $(CXXFLAGS_COMMON) -O3 -DNDEBUG $(ARCH_FLAGS) -flto
CXXFLAGS_DEBUG = $(CXXFLAGS_COMMON) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all debug test clean

all: $(EXE)

$(EXE): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS_RELEASE) $(SRCS) -o $@ -lpthread

debug: $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS_DEBUG) $(SRCS) -o $(EXE)-debug -lpthread

test: $(EXE)
	./$(EXE) suite
	./$(EXE) verify 3 "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"
	./$(EXE) epd tools/perft_random.epd
	./$(EXE) verifyepd tools/perft_random.epd 1

clean:
	rm -f $(EXE) $(EXE)-debug
