CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O0 -g3
 
tinyvm: main.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<
 
clean:
	rm -f tinyvm
 
.PHONY: clean