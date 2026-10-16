// tinyvm - Milestone 1 (within roadmap.md): the smallest useful KVM hypervisor.
//
// What this program does, in the same order a real VMM (QEMU, Firecracker,
// cloud-hypervisor) does it:
//   1. open /dev/kvm                      -> talk to the kernel's KVM module
//   2. KVM_CREATE_VM                      -> an empty "partition" (no CPU, no RAM)
//   3. mmap() anonymous memory            -> this becomes the guest's RAM
//   4. KVM_SET_USER_MEMORY_REGION         -> tell KVM: guest-physical 0x0.. == this host mapping
//   5. KVM_CREATE_VCPU + mmap(kvm_run)    -> a virtual CPU and the shared exit-info page
//   6. set registers, then loop KVM_RUN   -> CPU runs guest code natively (VMX/SVM)
//                                            until something needs the hypervisor
//   7. handle VM exits                    -> emulate a "serial port" at I/O port 0x3f8
//
// Build:  make        Run:  ./tinyvm      (needs Linux + access to /dev/kvm)

#include <fnctl.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#includ <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace{

    constexpr uint64_t kGuestRamSize = 1 << 20;         // 1MB of guest RAM
    constexpr uint64_t kCodeGpa         = 0x1000;       //where to load the guest code.(guest physical addr)
    constexpr uint64_t kMsgGpa          = 0x1010;       //where to put the message the guest prints
    constexpr uint16_t kSerialPort      = 0x3f8;        //classing COM1 I/O port

// Turn the "-1 + errno" into an exception so that the happy path stays readable.
int check(int rc, const char* what){
    if(rc < 0) throw std::system_error(errno, std::generic_category(), what);
    return rc;
}

// Tiny RAII wrapper. KVM hands us file descriptors for the system, the VM and each vCPU

class Fd {
    public:
        Fd() = default; // default ctor
        explicit Fd(int fd): fd_(fd){}
        Fd(Fd&& o) noexcept: fd_(std::exchange(o.fd_, -1)){} //copy constructor. we pick the o.fd_ and replace it with -1 atomically.
        Fd& operator=(Fd&&)
};



} // namespace




































