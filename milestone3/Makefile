CXX				?= 	g++
CXXFLAGS 		?= 	-std=c++17	-Wall	-Wextra	-O2
LDLIBS			?=	-pthread

all: tinyvm guest.bin spin.bin

tinyvm: main.cpp device.h devices.h kvm_util.h paging.h
	$(CXX) $(CXXFLAGS) -o $@ main.cpp $(LDLIBS)

# Guests are 64-bit flat binaries linked to run at guest-physical 0x1000
%.o: %.S
	as --64 -o $@ $<

%.bin: %.o
	ld -m elf_x86_64 -Ttext=0x1000 --oformat binary -o $@ $<

# Host-side tests: plain C++, no KVM required.
test_bus: test_bus.cpp device.h devices.h
	$(CXX) $(CXXFLAGS) -o $@ test_bus.cpp $(LDLIBS)

test_m3: test_m3.cpp devices.h paging.h kvm_util.h
	$(CXX) $(CXXFLAGS) -o $@ test_m3.cpp $(LDLIBS)

test: test_bus test_m3
	./test_bus
	./test_m3

# The same tests but under ThreadSanitizer which instruments every memory access and reports races
tsan: test_m3.cpp devices.h paging.h kvm_util.Host
	$(CXX) -std=c++17 -g -O1 -fsanitize=thread -o test_m3_tsan test_m3.cpp -pthread
	./test_m3_tsan

# Theading lessons 1 and 2 as a standalone program.
threads_demo: threads_demo.cpp
	$(CXX) $(CXXFLAGS) -o $@ threads_demo.cpp $(LDLIBS)

# Look at the machine code of the racy increment (and `lock xadd` in the atomic one).
disasm-demo: threads_demo
	objdump -d --no-show-raw-insn -C threads_demo | awk '/(racy|atomic)_worker.*>:$$/{p=1} p{print} /ret/{p=0}'

disasm: guest.bin
	objdump -D -b binary -m i386:x86_64 --adjust-vma=0x1000 guest.bin | less

run: all
	./tinyvm guest.bin

spin: all
	./tinyvm spin.bin 2000

clean:
	rm -f tinyvm test_bus test_m3 test_m3_tsan threads_demo *.o *.bin

.PHONY: all test tsan disasm-demo disasm run spin clean