CXX ?= g++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -O2 -g
CPPFLAGS ?= -Iinclude -DASIO_STANDALONE
LDFLAGS ?=
PKG_CONFIG ?= pkg-config
FFMPEG_PACKAGES := libavformat libavcodec libavutil libswscale
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(FFMPEG_PACKAGES) 2>/dev/null)
LDLIBS += $(shell $(PKG_CONFIG) --libs $(FFMPEG_PACKAGES) 2>/dev/null) -pthread

TARGET := rtsp-server
SOURCES := $(wildcard src/*.cpp)
OBJECTS := $(SOURCES:.cpp=.o)

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

-include $(OBJECTS:.o=.d)

clean:
	rm -f $(TARGET) src/*.o src/*.d

test: all
	python3 -m unittest discover -s tests -p '*_test.py'
