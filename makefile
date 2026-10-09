SHELL := /bin/sh
.SHELLFLAGS := -c

CXX := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -g
# Includes are written from these roots, for example "math/vec3.h" and "cgltf/cgltf.h".
CPPFLAGS := -Isrc -Iexternal
# GCC writes a .d file next to each object listing the headers it read, so a header change rebuilds exactly the
# files that include it. -MP adds an empty rule per header, so deleting a header does not break the build.
DEPFLAGS := -MMD -MP
LDLIBS := -lSDL3_image -lSDL3

GLSLC := glslc

# Build on every core, and keep each file's messages together instead of interleaved.
JOBS ?= $(shell nproc)
MAKEFLAGS += -j$(JOBS) --output-sync=target

TARGET := build/game.exe
TEST_TARGET := build/tests.exe
OBJECT_DIR := build/obj

# Recursive wildcard: $(call rwildcard,dir,pattern) finds matching files in dir and all its subfolders.
rwildcard = $(foreach entry,$(wildcard $(1:=/*)),$(call rwildcard,$(entry),$(2)) $(filter $(subst *,%,$(2)),$(entry)))

SOURCES := $(strip $(call rwildcard,src,*.cpp))
MAIN_SOURCE := src/app/main.cpp
ENGINE_SOURCES := $(filter-out $(MAIN_SOURCE),$(SOURCES))
TEST_SOURCES := $(wildcard tests/*.cpp)

# Each source compiles to its own object file, so a change recompiles only what it touches, and the game and the
# tests share the engine's objects instead of compiling the engine twice.
objects_for = $(patsubst %.cpp,$(OBJECT_DIR)/%.o,$(1))
ENGINE_OBJECTS := $(call objects_for,$(ENGINE_SOURCES))
MAIN_OBJECT := $(call objects_for,$(MAIN_SOURCE))
TEST_OBJECTS := $(call objects_for,$(TEST_SOURCES))
ALL_OBJECTS := $(ENGINE_OBJECTS) $(MAIN_OBJECT) $(TEST_OBJECTS)

# Find shader source files and their compiled output names.
SHADER_SOURCES := $(wildcard assets/shaders/*.vert assets/shaders/*.frag)
SHADER_BINARIES := $(addsuffix .spv,$(SHADER_SOURCES))

# Track whichever filename was used to load this Makefile.
MAKEFILE_PATH := $(lastword $(MAKEFILE_LIST))

.PHONY: all run test shaders clean

all: $(TARGET) shaders

# Linking only joins the compiled objects, which takes a moment.
$(TARGET): $(ENGINE_OBJECTS) $(MAIN_OBJECT)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(TEST_TARGET): $(ENGINE_OBJECTS) $(TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

# One object per source, mirroring the source folders under build/obj. Changing the makefile rebuilds everything,
# since the flags may have changed.
$(OBJECT_DIR)/%.o: %.cpp $(MAKEFILE_PATH)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# The header lists GCC wrote last time; missing on the first build, which compiles everything anyway.
-include $(ALL_OBJECTS:.o=.d)

# Compile each shader when its source changes or its output is missing.
shaders: $(SHADER_BINARIES)

assets/shaders/%.spv: assets/shaders/% $(MAKEFILE_PATH)
	$(GLSLC) $< -o $@

run: $(TARGET) shaders
	./$(TARGET)

test: $(TEST_TARGET) shaders
	./$(TEST_TARGET)

clean:
	rm -rf $(OBJECT_DIR) $(TARGET) $(TEST_TARGET) $(SHADER_BINARIES)
