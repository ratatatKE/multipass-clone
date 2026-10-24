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
#include <fstream>
#include <iterator>
#include <iostream>
#include <string>
#include <vector>

#include "device.h"
#include "devices.h"
#include "kvm_util.h"

using namespace tinyvm;

namespace
{

    constexpr uint64_t kGuestRamSize = 1 << 20;   // 1 MiB of RAM at guest-physical 0
    constexpr uint64_t kGuestEntry = 0x1000;      // guest.bin is loaded and started here
    constexpr uint16_t kUartPort = 0x3f8;         // COM1
    constexpr uint64_t kControlBase = 0xd0000000; // MMIO address, we put it very far above RAM (3,489,660,928). No memslot behind it.

    // The machine definition: two address spaces and the state the devices can poke
    struct Machine
    {
        Bus pio;  // port I/O space.
        Bus mmio; // memory-mapped I/O space
        bool powered_off = false;
        uint32_t exit_code = 0;
    };

    struct ExitStats
    {
        uint64_t pio_in = 0, pio_out = 0, mmio_read = 0, mmio_write = 0, hlt = 0;
        uint64_t total() const { return pio_in + pio_out + mmio_read + mmio_write + hlt; }
    };

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
            run_size_ = sz;
        }

        ~Vcpu()
        {
            if (run_)
                munmap(run_, run_size_);
        }
        Vcpu(const Vcpu &) = delete;
        Vcpu &operator=(const Vcpu &) = delete;

        /*
            Put the vCPU directly into flat 32-bit protected mode, as if a bootloader had already run.
            We fill in the *hidden* segment-descriptor caches directly, so no GDT has to exist in guest
            memory as long as the gest never reloads a segment register (ours doesn't).

            code: base 0, limit 4 GiB, 32-bit, execute/read
            data: base 0, limit 4 GiB, 32-bit, read/write
        */

        void reset_to_protected_mode(uint32_t entry_ip)
        {
            kvm_sregs sregs;
            check(ioctl(fd_.get(), KVM_GET_SREGS, &sregs), "KVM_GET_SREGS");

            // lambda function
            auto flat = [](kvm_segment &s, uint16_t selector, uint8_t type)
            {
                s.base = 0;
                s.limit = 0xffffffff;
                s.selector = selector;
                s.type = type;
                s.present = 1;
                s.dpl = 0;
                s.db = 1; // 32-bit
                s.s = 1;  // code/data segment (not a system segment)
                s.l = 0;
                s.g = 1; // limit counted in 4 KiB pages
                s.avl = 0;
                s.unusable = 0;
            };

            flat(sregs.cs, 0x08, 0xb); // execute/read, accessed
            flat(sregs.ds, 0x10, 0x3); // read/write, accesssed
            flat(sregs.es, 0x10, 0x3);
            flat(sregs.fs, 0x10, 0x3);
            flat(sregs.gs, 0x10, 0x3);
            flat(sregs.ss, 0x10, 0x3);
            sregs.cr0 |= 1; // CRO.PE i.e. protection enable flag. Paging stays off so that linear address = physical addresss.

            check(ioctl(fd_.get(), KVM_SET_SREGS, &sregs), "KVM_SET_SREGS");

            kvm_regs regs{};
            regs.rip = entry_ip;
            regs.rflags = 0x2; // bit 1 is reserved-set; IF-0, no interrupts
            check(ioctl(fd_.get(), KVM_SET_REGS, &regs), "KVM_SET_REGS");
        }

        void dump_state(const char *why)
        {
            kvm_regs r;
            kvm_sregs s;
            if (ioctl(fd_.get(), KVM_GET_REGS, &r) < 0 || ioctl(fd_.get(), KVM_GET_SREGS, &s) < 0)
                return;
            std::fprintf(stderr,
                         "%s\n  rip=0x%llx rflags=0x%llx cr0=0x%llx\n"
                         "  cs: sel=0x%x base=0x%llx limit=0x%x type=%u db=%u\n",
                         why, (unsigned long long)r.rip, (unsigned long long)r.rflags, (unsigned long long)s.cr0,
                         s.cs.selector, (unsigned long long)s.cs.base, s.cs.limit, s.cs.type, s.cs.db);
        }

        ExitStats run_loop(Machine &m)
        {

            ExitStats st;
            while (!m.powered_off)
            {

                if (ioctl(fd_.get(), KVM_RUN, 0) < 0)
                { // meaning an error has occured
                    if (errno == EINTR)
                        continue;
                    throw std::system_error(errno, std::generic_category(), "KVM_RUN");
                }

                switch (run_->exit_reason)
                {
                case KVM_EXIT_IO:
                {
                    /*
                        A single exit may carry several elements ( `rep outsb` ) hence `count`.
                    */

                    auto *data = reinterpret_cast<uint8_t *>(run_) + run_->io.data_offset;
                    const bool is_out = run_->io.direction == KVM_EXIT_IO_OUT;

                    for (uint32_t i = 0; i < run_->io.count; ++i)
                    {
                        uint8_t *p = data + static_cast<size_t>(i) * run_->io.size;

                        if (is_out)
                            m.pio.write(run_->io.port, p, run_->io.size);
                        else
                            m.pio.read(run_->io.port, p, run_->io.size);
                    }

                    ++(is_out ? st.pio_out : st.pio_in);
                    break;
                }

                case KVM_EXIT_MMIO:
                {
                    /*
                        For a read, whatever we leave in `data` beacoems the
                        value the guest's load instruction sees when we re-enter with KVM_RUN
                    */

                    auto &mm = run_->mmio;
                    if (mm.is_write)
                    {
                        m.mmio.write(mm.phys_addr, mm.data, mm.len);
                        ++st.mmio_write;
                    }
                    else
                    {
                        m.mmio.read(mm.phys_addr, mm.data, mm.len);
                        ++st.mmio_read;
                    }

                    break;
                }

                case KVM_EXIT_HLT:
                    ++st.hlt;
                    std::fprintf(stderr, "tinyvm: guest executed hlt without powering off \n");
                    return st;

                case KVM_EXIT_SHUTDOWN:
                    throw std::runtime_error("guest triple-falted (KVM_SHUTDOWN)");

                case KVM_EXIT_FAIL_ENTRY:
                    dump_state("internal error "); // Dump state of KVM so that we catch errrors faster.
                    throw std::runtime_error("VM entry failed, hardware reason 0x" + hex(run_->fail_entry.hardware_entry_failure_reason));

                case KVM_EXIT_INTERNAL_ERROR:
                    dump_state("internal error "); // Dump state of KVM so that we catch errrors faster.
                    throw std::runtime_error("KVM internal error, suberror " + std::to_string(run_->internal.suberror));

                default:
                    throw std::runtime_error("unhandled exit reason " + std::to_string(run_->exit_reason));
                }
            }

            return st;
        }

    private:
        static std::string hex(uint64_t v)
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%" PRIx64, v);
            return buf;
        }

        Fd fd_;
        kvm_run *run_ = nullptr;
        size_t run_size_ = 0;
    };

    // This looks like the function that will read our program. Glad we are using std functions because imagine doing this manually.
    std::vector<uint8_t> read_file(const std::string &path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            throw std::runtime_error("cannot open " + path + " (did you run make?)");
        /*
            std::vector<uint8_t> here: it’s a vector of plain bytes, not of function pointers. 
            istreambuf_iterator<char> walks the file one byte at a time, and the vector is constructed from that range. 
            It holds the raw contents of guest.bin, which then get copied into guest RAM.
        */
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); 
    }

    void report(const Machine &m, const ExitStats &st, std::chrono::nanoseconds elapsed)
    {
        std::printf("\n ------ vm stopped ----- \n");
        if (m.powered_off)
            std::printf("guest powered off with exit code %u\n", m.exit_code);

        std::printf("VM exits: %" PRIu64 " total \n", st.total());
        std::printf(" port I/O in/out : %" PRIu64 " / %" PRIu64 "\n", st.pio_in, st.pio_out);
        std::printf(" MMIO read/write : %" PRIu64 " / %" PRIu64 "\n", st.mmio_read, st.mmio_write);

        auto print_bus = [](const char *label, const Bus &bus)
        {
            for (const auto &[base, r] : bus.regions())
            {
                std::printf(" %s %-10s @ 0x%" PRIx64 " : %" PRIu64 " reads, %" PRIu64 " writes \n", label, r.dev->name(), base, r.reads, r.writes);
            }

            if (bus.unmapped_reads() || bus.unmapped_writes())
                std::printf(" %s unmapped   : %" PRIu64 " reads, %" PRIu64 " writes\n", label, bus.unmapped_reads(), bus.unmapped_writes());
        };

        print_bus("pio ", m.pio);
        print_bus("mmio", m.mmio);

        if (st.total() > 0)
        {
            const double us = std::chrono::duration<double, std::micro>(elapsed).count();
            std::printf("elapsed %.0f us => ~%.1f us per exit (includes our device code and stdout) \n", us, us / static_cast<double>(st.total()));
        }
    }

} // namespace #6.

int main(int argc, char **argv)
{

    try
    {
        const std::string guest_path = argc > 1 ? argv[1] : "guest.bin";

        // --------- KVM plumbing, same as milestone 1 --------------------
        Fd kvm(check(open("/dev/kvm", O_RDWR | O_CLOEXEC), "open /dev/kvm"));

        if (check(ioctl(kvm.get(), KVM_GET_API_VERSION, 0), "KVM_GET_API_VERSION") != 12)
            throw std::runtime_error("unexpected KVM API version (expected 12)");

        Fd vm(check(ioctl(kvm.get(), KVM_CREATE_VM, 0UL), "KVM_CREATE_VM"));

        /*
            RAM covers guest-physical [0 -> 1 MiB] ONLY. Every other address, including 0xd0000000, has no memslot so touching it makes the CPU exit wiht  KVM_EXIT_MMIO.
            "Is there a memslot here?" is the entire difference between RAM and a device. (NOTE THIS FOR INTERVIEW, MEMSLOT)

            These slots are saved in a kvm_userspace_memory_region object which is a KVM type.
        */
        GuestMemory ram(kGuestRamSize);
        kvm_userspace_memory_region region{};
        region.slot = 0;
        region.guest_phys_addr = 0;
        region.memory_size = ram.size();
        region.userspace_addr = reinterpret_cast<uint64_t>(ram.host_ptr(0));
        check(ioctl(vm.get(), KVM_SET_USER_MEMORY_REGION, &region), "KVM_SET_USER_MEMORY_REGION");

        // --- the new aprt: describe the machine ---
        Machine machine;
        Uart16550 uart(std::cout);
        ControlDevice control([&](uint32_t code)
                              {
            machine.powered_off = true; // the run loop checks this after each exit
            machine.exit_code = code; });

        machine.pio.attach(kUartPort, 8, uart);
        machine.mmio.attach(kControlBase, ControlDevice::kSize, control);

        // Load the guest "kernel" the way a VMM loads one: by reading bytes from a file into guest RAM.
        const std::vector<uint8_t> image = read_file(guest_path);
        ram.write(kGuestEntry, image.data(), image.size());

        Vcpu vcpu(vm.get(), kvm.get(), 0);
        vcpu.reset_to_protected_mode(kGuestEntry);

        const auto t0 = std::chrono::steady_clock::now();
        const ExitStats stats = vcpu.run_loop(machine);
        const auto elapsed = std::chrono::steady_clock::now() - t0;

        report(machine, stats, std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed));
        return 0;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "tinyvm: %s\n", e.what());
        return 1;
    }
}
