SHELL := /bin/sh
.SHELLFLAGS := -c

CXX := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -g
CPPFLAGS := -Iinclude -Isrc
LDLIBS := -lSDL3

TARGET := build/game.exe
TEST_TARGET := build/tests.exe

SOURCES := $(wildcard src/*.cpp)
ENGINE_SOURCES := $(filter-out src/main.cpp,$(SOURCES))
TEST_SOURCES := $(wildcard tests/*.cpp)
HEADERS := $(wildcard src/*.h include/*.h tests/*.h)

.PHONY: all run test clean

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS) Makefile | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) -o $(TARGET) $(LDLIBS)

$(TEST_TARGET): $(ENGINE_SOURCES) $(TEST_SOURCES) $(HEADERS) Makefile | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(ENGINE_SOURCES) $(TEST_SOURCES) -o $(TEST_TARGET) $(LDLIBS)

build:
	mkdir -p build

run: $(TARGET)
	./$(TARGET)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

clean:
	rm -f $(TARGET) $(TEST_TARGET)