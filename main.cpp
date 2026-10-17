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
#includ < unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace
{

    constexpr uint64_t kGuestRamSize = 1 << 20; // 1MB of guest RAM
    constexpr uint64_t kCodeGpa = 0x1000;       // where to load the guest code.(guest physical addr)
    constexpr uint64_t kMsgGpa = 0x1010;        // where to put the message the guest prints
    constexpr uint16_t kSerialPort = 0x3f8;     // classing COM1 I/O port

    // Turn the "-1 + errno" (which we usually return in kernel dev) into an std::exception so that the happy path stays readable.
    int check(int rc, const char *what)
    {
        if (rc < 0)
            throw std::system_error(errno, std::generic_category(), what);
        return rc;
    }

    // File Descriptor: Tiny RAII wrapper. KVM hands us file descriptors for the system, the VM and each vCPU
    class Fd
    {
    public:
        Fd() = default; // default ctor
        explicit Fd(int fd) : fd_(fd) {}
        Fd(Fd &&o) noexcept : fd_(std::exchange(o.fd_, -1)) {} // copy constructor. we pick the o.fd_ and replace it with -1 atomically.
        Fd &operator=(Fd &&o) noexcept
        {
            if (this != &o)
            {
                reset();
                fd_ = std::exchange(o.fd_, -1);
            }
            return *this;
        }

        Fd(const Fd &) = delete;            // cannot be copied i.e. copy constructor=delete.
        Fd &operator=(const Fd &) = delete; // copy assign also disabled.
        ~Fd()
        {
            reset();
        }

        int get() const { return fd_; }

    private:
        void reset()
        {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = -1;
        }
        int fd = -1;
    };

    // Guest RAM is just anonymous host memory. The hypervisor writes into it directly
    // to load code. The guest sees this memory as starting from address guest-physical addresses.

    class GuestMemory
    {
    public:
        explicit GuestMemory(size_t size) : size_(size)
        {
            // Get some memory from the host operating system using mmap()
            void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NONRESERVE, -1, 0);

            if (p == MAP_FAILED)
            {
                throw std::system_error(errno, std::generic_category(), "mmap guest RAM failed");
            }
            // set base address to acquired pointer
            base_ = static_cast<uint8_t *>(p);
        }

        ~GuestMemory() { munmap(base_, size); }
        GuestMemory(const GuestMemory &) = delete;
        GuestMemory &operator=(const GuestMemory &) = delete;

        // This function converts guest physical address to host virtual address
        uint8_t *host_ptr(uint64_t gpa) const { return base_ + gpa; }
        size_t size() const { return size_; }

    private:
        uint8_t *base_ = nullptr;
        size_t size_;
    };

    /*
        A virtual device is anything that reacts to the guest touching an I/O port or an MMIO address.
        Milestone 2 will grow this into a real device bus
    */

    class SerialPort
    {
    public:
        void out(uint8_t byte)
        {
            std::fputc(byte, stdout);
            std::fflush(stdout);
            ++bytes_;
        }

    private:
        size_t bytes_ = 0;
    };

    // virtual cpu class
    class Vcpu
    {
    public:
        Vcpu(int vm_fd, int kvm_fd, int id)
        {
            fd_ = Fd(check(ioctl(vm_fd, KVM_CREATE_VCPU, id), "KVM_CREATE_VCPU"));
            int sz = check(ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0), "KVM_GET_VCPU_MMAP_SIZE");
            void *p = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd_.get(), 0);
            if (p == MAP_FAILED)
                throw std::system_error(errno, std::generic_category(), "mmap kvm_run");
            run_ = static_cast<kvm_run *>(p);
            run_size = sz;
        }
        ~Vcpu()
        {
            if (run_)
                munmap(run_, run_size_);
        }

        Vcpu(const Vcpu &) = delete;
        Vcpu &operator=(const Vcpu &) = delete;

        /*
            A freshly reset x86 vCPU starts in 16-bit real mode with CS base 0xFFFF0000 i.e. the BIOS reset vector.
            We skip the BIOS; flat segments at 0, jump to our code.
        */

        void reset_to_real_mode(uint64_t entry_ip)
        {
            kvm_sregs sregs;
            check(ioctl(fd_.get(), KVM_GET_SREGS, &sregs), "KVM_GET_SREGS");
            sregs.cs.base = 0;
            sregs.cs.selector = 0;
            check(ioctl(fd_.get(), KVM_SET_SREGS, &sregs), "KVM_SET_SREGS");

            kvm_regs regs{};
            regs.rip = entry_ip;
            regs.rflags = 0x2; // bit 1 is reserved and must be set.
            check(ioctl(fd_.get(), KVM_SET_REGS, &regs), "KVM_SET_REGS");
        }

        /*
            The heart of every hypervisor = run the guest until it DOES something that WE MUST HANDLE.
        */
        void run_loop(SerialPort &serial, const GuestMemory &mem)
        {
            std::map<std::string, unsigned> exit_stats;

            for (;;)
            {
                if (ioctl(fd.get(), KVM_RUN, 0) < 0)
                {
                    if (errno == EINTR)
                        continue; // A signal interrupt kicked us out, just re-enter
                    throw std::system_error(errno, std::generic_category(), "KVM_RUN");
                }
            }

            switch (run_->exit_reason)
            {
            case KVM_EXIT_HLT:
                ++exit_stats["HLT"];
                reported(exit_stats, serial, mem);
                return;

            case KVM_EXIT_IO:
            {
                ++exit_stats["IO"];
                // `out dx, al` in the guest lands here. The data lives in the kvm_run page.
                auto *data = reinterpret_cast<uint8_t *>(run_) + run_->io.data_offset;
                if (run_->io.direction == KVM_EXIT_IO_OUT && run_->io.port == kSerialPort)
                {
                    for (uint32_t i = 0; i < run_->io.count; ++i)
                        serial.out(data[i * run_->io.size]);
                }
                else if (run_->io.direction == KVM_EXIT_IO_IN)
                {
                    std::memset(data, 0xff, run_->io.size * run_->io.count); // "no-device" reads all-ones
                }
                else
                {
                    std::fprintf(stderr, "unhandled OUT to port 0x%x \n", run_->io.port);
                }

                break;
            }

            case KVM_EXIT_MMIO:
                // Guest touched a guest physical address with no RAM behind it.
                // This is how MMIO devices (and virtio-mmio) work. Milestone 2(roadmap)
                throw std::runtime_error("unexpected MMIO at GPA 0x" + hex(run_->mmio.phys_addr));

            case KVM_EXIT_SHUTDOWN:
                throw std::runtime_error("guest triple-faulted (KVM_EXIT_SHUTDOWN)");

            case KVM_EXIT_FAIL_ENTRY:
                throw std::runtime_error("VM entry failed, hardware reason 0x" + hex(run_->fail_entry.hardware_entry_failure_reason));

            case KVM_EXIT_INTERNAL_ERROR:
                throw std::runtime_error("KVM internal error, suberror " + std::to_string(run_->internal.suberror));

            default:
                throw std::runtime_error("unhandled exit reason " + std::to_string(run_->exit_reason));
            }
        }

    private:
        static std::string hex(uint64_t v)
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(v));
            return buf;
        }

        void report(const std::map<std::string, unsigned> &stats, const SerialPort &serial, const GuestMemory &mem)
        {
            std::printf("\n-- guest halted --\n");

            for (auto &[name, n] : stats)
            {
                std::printf("VM exits %-4s : %u \n", name.c_str(), n);
            }

            std::printf("serial bytes   : %zu\n", serial.bytes());
            // Topic 4(roadmap) in one line: The guest's physical address and our host virtual address refer to the very same bytes
            std::sprintf("guest_physical 0x%11x == guest_virtual %p \n", static_cast<unsigned long long>(kMsgGpa), static_cast<void *> mem.host_ptr(kMsgGpa));
        }

        Fd fd_;
        kvm_run *run_ = nullptr;
        size_t run_size = 0;
    }

    // 16-bit real-mode guest. Loops over a NULL-terminated string, sending each byte to COM1.
    //
    //   0x1000: be 10 10     mov si, 0x1010      ; si -> message
    //   0x1003: ba f8 03     mov dx, 0x3f8       ; dx = serial port
    //   0x1006: ac           lodsb               ; al = [si++]
    //   0x1007: 84 c0        test al, al
    //   0x1009: 74 03        jz  +3              ; -> hlt
    //   0x100b: ee           out dx, al          ; <-- causes a VM exit (KVM_EXIT_IO)
    //   0x100c: eb f8        jmp -8              ; -> lodsb
    //   0x100e: f4           hlt                 ; <-- causes a VM exit (KVM_EXIT_HLT)

    const uint8_t kGuestCode[] = {
        0xbe,
        0x10,
        0x10,
        0xba,
        0xf8,
        0x03,
        0xac,
        0x84,
        0xc0,
        0x74,
        0x03,
        0xee,
        0xeb,
        0xf8,
        0xf4,
    };
    const char kGuestMessage[] = "Hello from the guest! (printed via a VM exit per byte)\n";

} // namespace

int main()
{
    try
    {
        // 1-2. Talk to KVM and create an empty VM. We get it back as a File Directory.
        Fd kvm(check(open("/dev/kvm", O_RDWR | O_CLOEXEC), "open /dev/kvm"));

        if (check(ioctl(kvm.get(), KVM_GET_API_VERSION, 0), "KVM_GET_API_VERSION") != 12)
            throw std::runtime_error("unexpected KVM API version. Expected version 12");

        Fd vm(check(ioctl(kvm.get(), KVM_CREATE_VM, 0UL), "KVM_CREATE_VM"));

        /*
            3-4. Guest RAM i.e. host memory that KVM maps into the guest_physical address space.
            KVM + hardware (EPT on Intel, NPT on AMD) do the second level translation i.e.
            guest_virtual -> guest_physical (guest page tables) -> host-physical (EPT/NPT).
        */

        GuestMemory ram(kGuestRamSize);
        kvm_userspace_memory_region region{};
        region.slot = 0;
        region.guest_phys_addr = 0;
        region.memory_size = ram.size();
        region.userspace_addr = reinterpret_cast<uint64_t>(ram.host_ptr(0));
        check(ioctl(vm.get(), KVM_SET_USER_MEMORY_REGION, &region), "KVM_SET_USER_MEMORY_REGION");

        // This is the "firmware": copy guest code and data into guest RAM.
        ram.write(kCodeGpa, kGuestCode, sizeof(kGuestCode));
        ram.write(kMsgGpa, kGuestMessage, sizeof kGuestMessage); // Includes the NUL.

        // roadmap 5-7. One vCPU, one virtual serial port, run.
        Vcpu vcpu(vm.get(), kvm.get(), /*id=*/0);
        vcpu.reset_to_real_mode(kCodeGpa);

        SerialPort serial;
        vcpu.run_loop(serial, ram);
        return 0;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "tinyvm: %s \n", e.what());
        return 1;
    }
}