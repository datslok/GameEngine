SHELL := /bin/sh
.SHELLFLAGS := -c

CXX := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -g
CPPFLAGS := -Iinclude

TARGET := build/game.exe
SOURCES := $(wildcard src/*.cpp)
HEADERS := $(wildcard src/*.h include/*.h)

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS) Makefile | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) -o $(TARGET)

build:
	mkdir -p build

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)