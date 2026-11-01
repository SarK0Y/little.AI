CXX      ?= c++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wpedantic
LDFLAGS  ?=

TARGETS  := voice_extractor voice_reconstruct
SOURCE   := voice_extractor.cpp
OBJECTS  := $(SOURCE:.cpp=.o)

.PHONY: all clean

all: $(TARGETS)

voice_extractor: $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ -lm

voice_reconstruct: template_reconstruct.o
	$(CXX) $(LDFLAGS) -o $@ $^ -lm

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	$(RM) $(TARGETS) $(OBJECTS) template_reconstruct.o
