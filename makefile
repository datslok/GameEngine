SHELL := /bin/sh
.SHELLFLAGS := -c

CXX := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -g
# Includes are written from these roots, for example "math/vec3.h" and "cgltf/cgltf.h".
CPPFLAGS := -Isrc -Iexternal
LDLIBS := -lSDL3_image -lSDL3

GLSLC := glslc

TARGET := build/game.exe
TEST_TARGET := build/tests.exe

# Recursive wildcard: $(call rwildcard,dir,pattern) finds matching files in dir and all its subfolders.
rwildcard = $(foreach entry,$(wildcard $(1:=/*)),$(call rwildcard,$(entry),$(2)) $(filter $(subst *,%,$(2)),$(entry)))

SOURCES := $(strip $(call rwildcard,src,*.cpp))
ENGINE_SOURCES := $(filter-out src/app/main.cpp,$(SOURCES))
TEST_SOURCES := $(wildcard tests/*.cpp)
HEADERS := $(strip $(call rwildcard,src,*.h) $(wildcard tests/*.h))

# Find shader source files and their compiled output names.
SHADER_SOURCES := $(wildcard assets/shaders/*.vert assets/shaders/*.frag)
SHADER_BINARIES := $(addsuffix .spv,$(SHADER_SOURCES))

# Track whichever filename was used to load this Makefile.
MAKEFILE_PATH := $(lastword $(MAKEFILE_LIST))

.PHONY: all run test shaders clean

all: $(TARGET) shaders

$(TARGET): $(SOURCES) $(HEADERS) $(MAKEFILE_PATH) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) -o $(TARGET) $(LDLIBS)

$(TEST_TARGET): $(ENGINE_SOURCES) $(TEST_SOURCES) $(HEADERS) $(MAKEFILE_PATH) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(ENGINE_SOURCES) $(TEST_SOURCES) -o $(TEST_TARGET) $(LDLIBS)

# Compile each shader when its source changes or its output is missing.
shaders: $(SHADER_BINARIES)

assets/shaders/%.spv: assets/shaders/% $(MAKEFILE_PATH)
	$(GLSLC) $< -o $@

build:
	mkdir -p build

run: $(TARGET) shaders
	./$(TARGET)

test: $(TEST_TARGET) shaders
	./$(TEST_TARGET)

clean:
	rm -f $(TARGET) $(TEST_TARGET) $(SHADER_BINARIES)