CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 $(shell sdl2-config --cflags)
LDFLAGS  = $(shell sdl2-config --libs) -lSDL2_ttf

TARGET   = memview
SRC      = memview.cpp

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)
