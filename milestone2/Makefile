CXX			?= g++
CXXFLAGS	?= -std=c++17 -Wall -Wextra -O0 -g3

all: tinyvm guest.bin

tinyvm: main.cpp device.h devices.h kvm_util.h
	$(CXX) $(CXXFLAGS) -o $@ main.cpp

# The guest is a flat binary linked to run at guest-physical 0x1000, like a tiny kernel image.
guest.o: guest.S
	as --32 -o $@ $<

guest.bin: guest.o
	ld -m elf_i386 -Ttext=0x1000 --oformat binary -o $@ $<

# Device-model tests: plain C++ to ensure no KVM required
test_bus: test_bus.cpp device.h devices.h
	$(CXX) $(CXXFLAGS) -o $@ test_bus.cpp

# Runs the test suite
test: test_bus
	./test_bus

#Show what the guest assembled to (handy for checking the encoding by eye)
disasm: guest.bin
	objdump -D -b binary -m i386 --adjust-vma=0x1000 guest.bin | less

run: all
	./tinyvm guest.bin

clean: 
	rm -f tinyvm test_bus guest.o guest.bin

.PHONY: all test disasm run clean