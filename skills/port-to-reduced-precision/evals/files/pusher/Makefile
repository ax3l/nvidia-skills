# SPDX-License-Identifier: Apache-2.0

CXX      := g++
CXXFLAGS := -O2 -std=c++17

EXEC := pusher

.PHONY: all run clean

all: $(EXEC)

$(EXEC): pusher.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

run: $(EXEC)
	./$(EXEC)

clean:
	rm -f *.o $(EXEC)
