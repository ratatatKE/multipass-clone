// tinyvm - Milestone 2: devices behind a bus.
//
// Milestone 1 hard-coded "if port == 0x3f8" inside the run loop. Real VMMs don't: they keep
// a machine description (which device lives at which address) and the run loop only
// translates "KVM exit" into "bus access". That is what this milestone adds:
//
//   guest `out`/`in`  --KVM_EXIT_IO-->   pio bus  --> Uart16550
//   guest load/store  --KVM_EXIT_MMIO--> mmio bus --> ControlDevice
//
// Build: make        Run: ./tinyvm [guest.bin]      Test the bus without KVM: make test

#include <fcntl.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include<fstream>
#include <iterator>
#include <iostream>
#include <string>
#include <vector>

#include "device.h"
#include "devices.h"
#include "kvm_util.h";

using namespace tinyvm;


namespace{
































    


} // namespace