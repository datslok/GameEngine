SHELL := /bin/sh
.SHELLFLAGS := -c

CXX := g++
CC := gcc

# Two builds from the same sources: debug (the default, unoptimised so a debugger sees every line and variable) and
# release (make release, -O2, for playing and measuring). Each has its own folder, so switching never rebuilds the other.
# Debug symbols (-g) stay in both: they cost nothing at run time, and packaging strips them.
BUILD ?= debug
ifeq ($(BUILD),release)
    OPTIMISATION := -O2
    BUILD_DIR := build/release
else ifeq ($(BUILD),debug)
    OPTIMISATION := -O0
    BUILD_DIR := build
    # Read through core/build_info.h (isDebugBuild), for debug-only defaults such as GPU validation.
    BUILD_DEFINES := -DENGINE_DEBUG_BUILD
else
    $(error BUILD must be debug or release, not $(BUILD))
endif

# Never -DNDEBUG (the tests rely on assert) or -ffast-math (it changes floating-point results and breaks NaN checks).
CXXFLAGS := -std=c++20 -Wall -Wextra -g $(OPTIMISATION) $(BUILD_DEFINES)
# Includes are written from these roots, for example "math/vec3.h" and "cgltf/cgltf.h". VK_NO_PROTOTYPES: every Vulkan
# function is a pointer volk fetches from the driver at run time (external/volk), never a symbol linked from the loader.
CPPFLAGS := -Isrc -Iexternal -DVK_NO_PROTOTYPES
# GCC writes a .d file next to each object listing the headers it read, so a header change rebuilds exactly the
# files that include it. -MP adds an empty rule per header, so deleting a header does not break the build.
DEPFLAGS := -MMD -MP
LDLIBS := -lSDL3_image -lSDL3

GLSLC := glslc
# The renderer is Vulkan 1.3, so its shaders may use what that guarantees (SPIR-V 1.6; subgroup operations need 1.3).
GLSLC_TARGET := --target-env=vulkan1.3

# The renderer's Vulkan validation layers (debug builds): MSYS2 installs them beside its DLLs, where the Vulkan loader
# does not look on its own, so every program make runs is told the folder.
export VK_ADD_LAYER_PATH ?= $(shell cygpath -w /ucrt64/bin 2>/dev/null)

# Build on every core, and keep each file's messages together instead of interleaved.
JOBS ?= $(shell nproc)
MAKEFLAGS += -j$(JOBS) --output-sync=target

TARGET := $(BUILD_DIR)/game.exe
TEST_TARGET := $(BUILD_DIR)/tests.exe
GPU_TEST_TARGET := $(BUILD_DIR)/gputests.exe
OBJECT_DIR := $(BUILD_DIR)/obj

# Recursive wildcard: $(call rwildcard,dir,pattern) finds matching files in dir and all its subfolders.
rwildcard = $(foreach entry,$(wildcard $(1:=/*)),$(call rwildcard,$(entry),$(2)) $(filter $(subst *,%,$(2)),$(entry)))

SOURCES := $(strip $(call rwildcard,src,*.cpp))
MAIN_SOURCE := src/app/main.cpp
# Vendored libraries compiled with the engine: Dear ImGui (the debug UI) and its SDL3 and Vulkan backends,
# vk-bootstrap (Vulkan device set-up) and the Vulkan Memory Allocator, and in C, MikkTSpace (tangents for normal maps)
# and volk (Vulkan function loading).
EXTERNAL_SOURCES := $(wildcard external/imgui/*.cpp) $(wildcard external/vk-bootstrap/*.cpp) $(wildcard external/vma/*.cpp)
EXTERNAL_C_SOURCES := $(wildcard external/mikktspace/*.c) $(wildcard external/volk/*.c)
ENGINE_SOURCES := $(filter-out $(MAIN_SOURCE),$(SOURCES)) $(EXTERNAL_SOURCES)
TEST_SOURCES := $(wildcard tests/*.cpp)
# The GPU tests (make gputest) are a program of their own, so make test keeps running without a GPU.
GPU_TEST_SOURCES := $(wildcard tests/gpu/*.cpp)

# Each source compiles to its own object file, so a change recompiles only what it touches, and the game and the
# tests share the engine's objects instead of compiling the engine twice.
objects_for = $(patsubst %.cpp,$(OBJECT_DIR)/%.o,$(1))
ENGINE_OBJECTS := $(call objects_for,$(ENGINE_SOURCES)) $(patsubst %.c,$(OBJECT_DIR)/%.o,$(EXTERNAL_C_SOURCES))
MAIN_OBJECT := $(call objects_for,$(MAIN_SOURCE))
TEST_OBJECTS := $(call objects_for,$(TEST_SOURCES))
GPU_TEST_OBJECTS := $(call objects_for,$(GPU_TEST_SOURCES))
ALL_OBJECTS := $(ENGINE_OBJECTS) $(MAIN_OBJECT) $(TEST_OBJECTS) $(GPU_TEST_OBJECTS)

# Find shader source files and their compiled output names.
SHADER_SOURCES := $(wildcard assets/shaders/*.vert assets/shaders/*.frag assets/shaders/*.comp)
SHADER_BINARIES := $(addsuffix .spv,$(SHADER_SOURCES))
# The GPU tests' compute shaders, which include the real shader libraries from assets/shaders.
GPU_TEST_SHADER_SOURCES := $(wildcard tests/gpu/shaders/*.comp)
GPU_TEST_SHADER_BINARIES := $(addsuffix .spv,$(GPU_TEST_SHADER_SOURCES))

# Track whichever filename was used to load this Makefile.
MAKEFILE_PATH := $(lastword $(MAKEFILE_LIST))

.PHONY: all run test shaders clean release run-release test-release golden golden-save gputest gputest-release

all: $(TARGET) shaders

# Linking only joins the compiled objects, which takes a moment.
$(TARGET): $(ENGINE_OBJECTS) $(MAIN_OBJECT)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

# GCC's -O2 range analysis reports false array-bounds warnings in ImGui (an index it cannot prove stays below 5). Silenced
# for the vendored files only; the engine's own code keeps every warning.
$(call objects_for,$(EXTERNAL_SOURCES)): CXXFLAGS += -Wno-array-bounds

# The Vulkan Memory Allocator leaves parameters unused in some configurations and initialises Vulkan structs partly
# (the rest zero, as intended). Silenced for it only.
$(OBJECT_DIR)/external/vma/vk_mem_alloc.o: CXXFLAGS += -Wno-unused-parameter -Wno-unused-variable -Wno-missing-field-initializers

# ImGui's Vulkan backend calls Vulkan through volk, which it includes as <volk.h>.
$(OBJECT_DIR)/external/imgui/imgui_impl_vulkan.o: CPPFLAGS += -DIMGUI_IMPL_VULKAN_USE_VOLK -Iexternal/volk

$(TEST_TARGET): $(ENGINE_OBJECTS) $(TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(GPU_TEST_TARGET): $(ENGINE_OBJECTS) $(GPU_TEST_OBJECTS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

# One object per source, mirroring the source folders under build/obj. Changing the makefile rebuilds everything,
# since the flags may have changed.
$(OBJECT_DIR)/%.o: %.cpp $(MAKEFILE_PATH)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

# The vendored C files, as C99 with the same optimisation. MikkTSpace sets two counters it never reads; that warning is
# silenced for it only.
$(OBJECT_DIR)/%.o: %.c $(MAKEFILE_PATH)
	@mkdir -p $(@D)
	$(CC) -std=c99 -Wall -Wextra -Wno-unused-but-set-variable -g $(OPTIMISATION) $(DEPFLAGS) -c $< -o $@

# The header lists GCC wrote last time; missing on the first build, which compiles everything anyway.
-include $(ALL_OBJECTS:.o=.d)

# Compile each shader when its source, a header it includes, or the makefile changes. Shaders include the headers they
# share with C++ from the same src root (-Isrc), and glslc writes the list of what each one read to build/obj/shaders.
# The compiled shaders are shared by the debug and release builds, so their lists are too (in a per-build folder, the
# other build would not know what its shaders depend on and keep stale ones).
SHADER_DEPENDENCY_DIR := build/obj/shaders

shaders: $(SHADER_BINARIES)

assets/shaders/%.spv: assets/shaders/% $(MAKEFILE_PATH)
	@mkdir -p $(SHADER_DEPENDENCY_DIR)
	$(GLSLC) $(GLSLC_TARGET) -Isrc -MD -MF $(SHADER_DEPENDENCY_DIR)/$(notdir $@).d $< -o $@

-include $(patsubst assets/shaders/%,$(SHADER_DEPENDENCY_DIR)/%.d,$(SHADER_BINARIES))

# The GPU tests' shaders also include the shader libraries (assets/shaders) and the function numbers (tests/gpu).
GPU_TEST_SHADER_DEPENDENCY_DIR := $(SHADER_DEPENDENCY_DIR)/gpu_tests

tests/gpu/shaders/%.spv: tests/gpu/shaders/% $(MAKEFILE_PATH)
	@mkdir -p $(GPU_TEST_SHADER_DEPENDENCY_DIR)
	$(GLSLC) $(GLSLC_TARGET) -Isrc -Iassets/shaders -Itests/gpu -MD -MF $(GPU_TEST_SHADER_DEPENDENCY_DIR)/$(notdir $@).d $< -o $@

-include $(patsubst tests/gpu/shaders/%,$(GPU_TEST_SHADER_DEPENDENCY_DIR)/%.d,$(GPU_TEST_SHADER_BINARIES))

run: $(TARGET) shaders
	./$(TARGET)

test: $(TEST_TARGET) shaders
	./$(TEST_TARGET)

# Golden images (src/engine/golden.h): golden-save renders every game's fixed views as the references in
# tests/golden/<GPU>/<game>/, golden compares fresh captures with them (captures and difference heatmaps go to
# build/golden/<GPU>/<game>/). They need a GPU.
golden: $(TARGET) shaders
	./$(TARGET) --golden-check

golden-save: $(TARGET) shaders
	./$(TARGET) --golden-save

# The GPU test tier (tests/gpu): every shader library's functions run in compute shaders on fixed inputs and compared
# with their C++ mirrors, then the golden images. Needs a GPU, so it is not part of make test.
gputest: $(GPU_TEST_TARGET) $(GPU_TEST_SHADER_BINARIES) $(TARGET) shaders
	./$(GPU_TEST_TARGET)
	./$(TARGET) --golden-check

# The release build through the same rules, in build/release. Running the tests optimised as well catches bugs that only
# show at -O2 (uninitialised variables, code that breaks the language rules the optimiser relies on).
release:
	$(MAKE) BUILD=release all

run-release:
	$(MAKE) BUILD=release run

test-release:
	$(MAKE) BUILD=release test

gputest-release:
	$(MAKE) BUILD=release gputest

clean:
	rm -rf build/obj build/game.exe build/tests.exe build/gputests.exe build/release $(SHADER_BINARIES) $(GPU_TEST_SHADER_BINARIES)
