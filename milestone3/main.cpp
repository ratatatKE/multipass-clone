/*

tinyvm - Milestone 3: long mode + page tables + the vCPU on its own thread.

Two themes which meet in this file:

A. MEMORY. 
----------------------------
The guest now runs in 64-bit mode with paging ON. We build its page tables in the guest RAM ourselves (paging.h),
point CR3 at them, and afterwards ask "what does guest-virtual address X really mean?".

Three different meanings depending on three different processes below:
- Our own sotware page walk,
- KVM_TRANSLATE (the kernel walking the same page tables)
- the host-virtual and host-physical addresses behind it.

That is the whole GVA -> GPA -> HVA -> HPA chain.

B. THREADS.
----------------------------
A real VMM runs each virtual CPU on its own OS thread. We do the same and keep the main thread as a supervisor with a 
watchdog timer. This forcess us to learn about std::thread, std::atomic, std::mutex and std::condition_variable for real reasons.

To build this do:
------------------
make 

To run it do: 
------------------
./tinyvm [guest.bin] [timeout_ms]              
for example `./tinyvm spin.bin 2000` 

*/

#include <fcntl.h>
#include <linux/kvm.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "device.h"
#include "devices.h"
#include "kvm_util.h"
#include "paging.h"

using namespace tinyvm;

namespace{

constexpr uint64_t kGuestRamSize                = 1 << 20;              // 1 MiB of RAM at guest-physical 0
constexpr uint64_t kGuestEntry                  = 0x1000;               // guest.bin is loaded and started here
constexpr uint16_t kUartPort                    = 0x3f8;                // COM1

// Control-register bits we need to enter 64-bit mode (Intel SDM vol. 3, ch. 2 and 4)
constexpr uint64_t kCr0Pe = 1ull << 0;                                  // protected mode enable
constexpr uint64_t kCr0Mp = 1ull << 1;                                  
constexpr uint64_t kCr0Et = 1ull << 4;
constexpr uint64_t kCr0Ne = 1ull << 5;
constexpr uint64_t kCr0Wp = 1ull << 16;
constexpr uint64_t kCr0Am = 1ull << 18;
constexpr uint64_t kCr0Pg = 1ull << 31;                                 // paging enable
constexpr uint64_t kCr4Pae= 1ull << 5;                                  // physical address extension: required for long mode
constexpr uint64_t kEferLme = 1ull << 8;                                // long mode ENABLE (we ask for it)
constexpr uint64_t kEferLma = 1ull << 10;                               // long mode ACTIVE (the CPU reports it)


struct Machine {
    Bus pio;
    Bus mmio;
    bool powered_off = false;                                           // touched ONLY by the vCPU thread while the VM runs (see primer)
    uint32_t exit_code = 0;
};

struct ExitStats {
    uint64_t pio_in = 0, pio_out=0, mmio_read=0, mmio_write=0, hlt=0;
    uint64_t total() const { return pio_in + pio_out + mmio_read + mmio_write + hlt; }
};

/*

	===========================================================================================
								 THREADING PRIMER (lessons 1, 2, 4, 5)
	  Lesson 3 (std::mutex / lock_guard) is in devices.h, next to the UART it protects.
	  Lessons 1 and 2 are also runnable on their own: see threads_demo.cpp.
	===========================================================================================

	LESSON 1 - WHAT A THREAD IS, TO THE MACHINE
	-------------------------------------------
	A CPU core executes one instruction stream: RIP says where, RSP says where the stack is,
	and the general registers hold the values in flight. A THREAD is the kernel's name for one
	such saved set (RIP, RSP, registers) plus a private stack. The kernel's scheduler loads a
	thread's registers onto a core, lets it run, and later swaps in another. Threads of one
	process share EVERYTHING else: one address space (same CR3, same page tables), so every
	global variable, the heap, and every pointer we pass is visible to all of them. That sharing is
	both the point and the danger: two cores can read and write the same bytes at the same time.

	std::thread t(f, args...) asks the kernel (pthread_create, which calls clone() with the
	"share the address space" flags) to start a new thread running f(args...) immediately.
	The new thread may run before the constructor even returns. Rules to remember:
	 * Everything the constructing thread wrote BEFORE creating the thread is guaranteed to be
	   visible to the new thread. (The standard promises this: creation synchronizes-with start.)
	   We rely on it: main() fills guest RAM and registers before starting the vCPU thread.
	 * A std::thread object that is still "joinable" when destroyed calls std::terminate(),
	   killing the whole process. Reason: the thread might still be using stack variables of
	   the scope that is being destroyed; the language refuses to guess. So always t.join()
	   (wait for it to finish) or t.detach() (let it run unsupervised). C++20 adds
	   std::jthread, which joins in its destructor; we are on C++17, so we join by hand.
	 * join() also gives you a guarantee: everything the thread wrote before it finished is
	   visible to you after join() returns. That is why main() may read the Machine and the
	   statistics after joining, with no lock at all.

	LESSON 2 - ATOMICS, AND WHY A PLAIN bool IS NOT ENOUGH
	------------------------------------------------------
	Suppose thread A loops `while (!stop) {...}` and thread B sets `stop = true`.
	With a plain bool there are TWO independent ways this fails:
	 (a) The COMPILER may read `stop` once, keep it in a register, and never reread it,
		 because within a single thread nothing in the loop changes it. The loop then never ends.
	 (b) The CPU has a STORE BUFFER: a store first goes into a small private queue and becomes
		 visible to other cores a little later. And cores may complete loads/stores in a
		 different order than the program wrote them. x86 is fairly strict about this ("TSO":
		 the only reordering is a store appearing later than a following load); ARM is looser.
	`std::atomic<bool>` fixes both. It tells the compiler "every access really touches memory,
	and do not reorder other memory accesses across me", and it emits the CPU instructions
	that enforce ordering. On x86 a seq_cst store becomes `xchg` (an implicitly locked
	instruction that also drains the store buffer) and a load is a plain `mov`.
	Read-modify-write operations such as counter.fetch_add(1) become `lock xadd` (or `lock add`): the `lock`
	prefix makes the core own the cache line exclusively for the whole read+add+write, so no
	other core can slip between them. (`volatile` does NOT do this: it only handles (a).)
	The default memory order, seq_cst, is the strongest and simplest. Weaker orders
	(acquire/release/relaxed) are an optimisation to learn later.

	LESSON 4 - CONDITION VARIABLES: SLEEPING UNTIL SOMETHING HAPPENS
	----------------------------------------------------------------
	The watchdog must wait "until the VM finishes OR 5 seconds pass". Spinning on a flag burns
	a whole core. Instead it sleeps in the kernel (futex wait) and is woken by whoever finishes.
	The trap is the LOST WAKEUP: if the watchdog checks `finished` (false), and BEFORE it
	manages to go to sleep the vCPU thread sets finished = true and calls notify, the notify
	finds nobody sleeping and is lost; the watchdog then sleeps until the timeout for nothing.
	The cure is that "check the flag and go to sleep" must be atomic with respect to "set the
	flag and notify". A std::condition_variable does it with a mutex:
	  * the waiter locks the mutex, checks the flag, and cv.wait() atomically UNLOCKS the mutex
		and goes to sleep (so a notifier can get the lock, set the flag, notify);
	  * on wakeup it RE-LOCKS the mutex before returning.
	That is why wait() takes a std::unique_lock (a lock_guard that can be unlocked and relocked
	by the condition variable) and why the flag is changed while holding the same mutex.
	Wakeups can also be SPURIOUS (the kernel may wake you for no reason), so you never write
	`cv.wait(lk);` bare. You pass a predicate, `[&]{ return finished; }`, and wait() re-checks
	it in a loop: sleep, wake, check, maybe sleep again. wait_for adds a timeout.

	LESSON 5 - THE REST OF THE LIFECYCLE: EXCEPTIONS AND SIGNALS
	------------------------------------------------------------
	 * Exceptions do not cross threads. If an exception escapes the function a std::thread
	   runs, there is no caller above it to catch it: std::terminate() kills the process. So
	   the thread function catches everything, stores it as a std::exception_ptr (a handle to
	   the in-flight exception object), and the joiner rethrows it with std::rethrow_exception.
	 * KVM_RUN blocks inside the kernel for as long as the guest runs without needing us. A
	   guest in an endless `jmp` loop therefore never lets our loop test any flag. The only
	   way to interrupt a blocked system call is a SIGNAL: the kernel makes the call return
	   -1 with errno == EINTR. We send one with pthread_kill(vcpu_thread, SIGUSR1).
	 * The lost-wakeup problem strikes again: if the signal arrives while the vCPU thread is
	   in OUR code (between the loop test and the ioctl), the handler just runs and returns;
	   the thread then enters KVM_RUN afterwards and sleeps forever. KVM gives us a flag for
	   exactly this: kvm_run.immediate_exit. We set it BEFORE sending the signal; KVM_RUN
	   checks it on entry and returns EINTR at once. Now either order works: signal while in
	   the syscall interrupts it, signal before the syscall is caught by immediate_exit.
	   (The flag lives in memory shared with the kernel, so we write it with an atomic store.)
	===========================================================================================

	The signal handler does nothing at all. Its only job is to exist: a signal whose action is
	"ignore" does not interrupt blocked system calls, but a signal with a handler does.

*/

extern "C" void on_kick_signal(int){
    // has been implemented externally
}

void install_kick_handler(){
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_kick_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // deliberately not SA_RESTART: we want the ioctl to fail with EINTR
    check(sigaction(SIGUSR1, &sa, nullptr), "sigaction");
}

class Vcpu {
  public:
    Vcpu(int vm_fd, int kvm_fd, int id) {
        fd_ = Fd(check(ioctl(vm_fd, KVM_CREATE_VCPU, id), "KVM_CREATE_VCPU"));
        int sz = check(ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0), "KVM_GET_VCPU_MMAP_SIZE");
        void* p = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd_.get(), 0);
        if (p == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "mmap kvm_run");
        run_ = static_cast<kvm_run*>(p);
        run_size_ = sz;
    }
    ~Vcpu() { if (run_) munmap(run_, run_size_); }
    Vcpu(const Vcpu&) = delete;
    Vcpu& operator=(const Vcpu&) = delete;
 
    // Enter 64-bit long mode directly, as if firmware and a bootloader had already run.
    // The hardware rules (Intel SDM vol. 3, "IA-32e mode"): CR4.PAE must be 1, CR3 must point
    // at a valid PML4, then EFER.LME=1 and CR0.PG=1 make the CPU set EFER.LMA=1 (we write
    // LMA ourselves because we skip the CPU's normal transition). The code segment must have
    // L=1 (64-bit) and D=0. Segment bases and limits are ignored for data in 64-bit mode.
    void reset_to_long_mode(uint64_t entry_ip, uint64_t cr3) {
        kvm_sregs sregs;
        check(ioctl(fd_.get(), KVM_GET_SREGS, &sregs), "KVM_GET_SREGS");
 
        auto seg = [](kvm_segment& s, uint16_t selector, uint8_t type, bool code64) {
            s.base = 0;
            s.limit = 0xffffffff;
            s.selector = selector;
            s.type = type;
            s.present = 1;
            s.dpl = 0;
            s.db = code64 ? 0 : 1;   // for 64-bit code D must be 0
            s.s = 1;
            s.l = code64 ? 1 : 0;    // L=1: 64-bit code segment
            s.g = 1;
            s.avl = 0;
            s.unusable = 0;
        };
        seg(sregs.cs, 0x08, 0xb, true);   // execute/read, accessed
        seg(sregs.ds, 0x10, 0x3, false);  // read/write, accessed
        seg(sregs.es, 0x10, 0x3, false);
        seg(sregs.fs, 0x10, 0x3, false);
        seg(sregs.gs, 0x10, 0x3, false);
        seg(sregs.ss, 0x10, 0x3, false);
 
        sregs.cr3 = cr3;
        sregs.cr4 = kCr4Pae;
        sregs.cr0 = kCr0Pe | kCr0Mp | kCr0Et | kCr0Ne | kCr0Wp | kCr0Am | kCr0Pg;
        sregs.efer = kEferLme | kEferLma;
        check(ioctl(fd_.get(), KVM_SET_SREGS, &sregs), "KVM_SET_SREGS");
 
        kvm_regs regs{};
        regs.rip = entry_ip;
        regs.rflags = 0x2;
        check(ioctl(fd_.get(), KVM_SET_REGS, &regs), "KVM_SET_REGS");
    }
 
    // Ask KVM to walk the GUEST's page tables for us (KVM_TRANSLATE). Valid once the vCPU is
    // not running. Returns the guest-physical address, or nothing if the guest would #PF.
    std::optional<uint64_t> translate(uint64_t gva) {
        kvm_translation t{};
        t.linear_address = gva;
        check(ioctl(fd_.get(), KVM_TRANSLATE, &t), "KVM_TRANSLATE");
        if (!t.valid) return std::nullopt;
        return t.physical_address;
    }
 
    // Called from ANOTHER thread to make run_loop() return. Step 1 of 2; the caller must
    // then pthread_kill() the vCPU thread (lesson 5).
    void request_stop() {
        stop_.store(true);  // seq_cst atomic store: the vCPU thread's loop test will see it
        __atomic_store_n(&run_->immediate_exit, 1, __ATOMIC_SEQ_CST);  // same, for the kernel's copy
    }
    bool stop_requested() const { return stop_.load(); }
 
    // Runs on the vCPU thread.
    ExitStats run_loop(Machine& m) {
        ExitStats st;
        while (!m.powered_off && !stop_.load()) {
            if (ioctl(fd_.get(), KVM_RUN, 0) < 0) {
                if (errno == EINTR) continue;  // the loop condition decides: stop, or just a stray signal
                throw std::system_error(errno, std::generic_category(), "KVM_RUN");
            }
 
            switch (run_->exit_reason) {
                case KVM_EXIT_IO: {
                    auto* data = reinterpret_cast<uint8_t*>(run_) + run_->io.data_offset;
                    const bool is_out = run_->io.direction == KVM_EXIT_IO_OUT;
                    for (uint32_t i = 0; i < run_->io.count; ++i) {
                        uint8_t* p = data + static_cast<size_t>(i) * run_->io.size;
                        if (is_out) m.pio.write(run_->io.port, p, run_->io.size);
                        else        m.pio.read(run_->io.port, p, run_->io.size);
                    }
                    ++(is_out ? st.pio_out : st.pio_in);
                    break;
                }
 
                case KVM_EXIT_MMIO: {
                    auto& mm = run_->mmio;
                    if (mm.is_write) { m.mmio.write(mm.phys_addr, mm.data, mm.len); ++st.mmio_write; }
                    else             { m.mmio.read(mm.phys_addr, mm.data, mm.len);  ++st.mmio_read; }
                    break;
                }
 
                case KVM_EXIT_INTR:  // KVM_RUN returned because of a signal; loop condition decides
                    break;
 
                case KVM_EXIT_HLT:
                    ++st.hlt;
                    std::fprintf(stderr, "tinyvm: guest executed hlt without powering off\n");
                    return st;
 
                case KVM_EXIT_SHUTDOWN:
                    // With paging on and no IDT, a page fault becomes a double fault becomes a
                    // triple fault, which x86 reports as "shutdown". If you see this, the guest
                    // touched a virtual address its own page tables do not map.
                    dump_state("triple fault (KVM_EXIT_SHUTDOWN)");
                    throw std::runtime_error("guest triple-faulted (probably an unmapped virtual address)");
 
                case KVM_EXIT_FAIL_ENTRY:
                    dump_state("VM entry failed");
                    throw std::runtime_error("VM entry failed, hardware reason 0x" +
                                             hex(run_->fail_entry.hardware_entry_failure_reason));
 
                case KVM_EXIT_INTERNAL_ERROR:
                    dump_state("KVM internal error");
                    throw std::runtime_error("KVM internal error, suberror " +
                                             std::to_string(run_->internal.suberror));
 
                default:
                    throw std::runtime_error("unhandled exit reason " + std::to_string(run_->exit_reason));
            }
        }
        return st;
    }
 
  private:
    static std::string hex(uint64_t v) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%" PRIx64, v);
        return buf;
    }
 
    // The guest's registers live inside KVM, not in our process, so gdb cannot show them.
    // This is how you look at them.
    void dump_state(const char* why) {
        kvm_regs r;
        kvm_sregs s;
        if (ioctl(fd_.get(), KVM_GET_REGS, &r) < 0 || ioctl(fd_.get(), KVM_GET_SREGS, &s) < 0) return;
        std::fprintf(stderr,
                     "%s\n  rip=0x%" PRIx64 " rsp=0x%" PRIx64 " rflags=0x%" PRIx64 "\n"
                     "  cr0=0x%" PRIx64 " cr3=0x%" PRIx64 " cr4=0x%" PRIx64 " efer=0x%" PRIx64 "\n"
                     "  cs: sel=0x%x base=0x%" PRIx64 " type=%u l=%u db=%u\n",
                     why, uint64_t(r.rip), uint64_t(r.rsp), uint64_t(r.rflags), uint64_t(s.cr0),
                     uint64_t(s.cr3), uint64_t(s.cr4), uint64_t(s.efer), s.cs.selector,
                     uint64_t(s.cs.base), s.cs.type, s.cs.l, s.cs.db);
    }
 
    Fd fd_;
    kvm_run* run_ = nullptr;
    size_t run_size_ = 0;
    std::atomic<bool> stop_{false};
};
 
// State shared between the vCPU thread and the supervisor (main) thread.
// RULE: every member is protected by `mu`, except `stats` and `error`, which the vCPU
// thread writes BEFORE it takes `mu` to set `finished`; main reads them only AFTER it has
// seen finished == true under `mu` (and after join()). The mutex hand-off orders those
// accesses, so they need no lock of their own. That reasoning ("who writes when, and what
// orders it") is how you decide where locks are needed. If you cannot write that sentence
// for a variable, it needs a lock or an atomic.
struct Shared {
    std::mutex mu;
    std::condition_variable cv;
    bool finished = false;          // protected by mu
    ExitStats stats;                // vCPU thread writes, then publishes via `finished`
    std::exception_ptr error;       // same
};
 
// Body of the vCPU thread. NOTHING may escape this function as an exception (lesson 5).
void vcpu_thread_main(Vcpu& vcpu, Machine& machine, Shared& shared) {
    try {
        shared.stats = vcpu.run_loop(machine);
    } catch (...) {
        shared.error = std::current_exception();  // smuggle the exception to the joiner
    }
    {
        std::lock_guard<std::mutex> lk(shared.mu);
        shared.finished = true;                    // change the condition while holding the mutex...
    }
    shared.cv.notify_all();                        // ...then wake the waiter (lesson 4)
}
 
std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path + " (did you run make?)");
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
 
void report(const Machine& m, const ExitStats& st, std::chrono::nanoseconds elapsed, bool by_watchdog) {
    std::printf("\n--- vm stopped ---\n");
    if (by_watchdog) std::printf("stopped by the watchdog (the guest did not finish in time)\n");
    if (m.powered_off) std::printf("guest powered off with exit code %u\n", m.exit_code);
 
    std::printf("VM exits: %" PRIu64 " total\n", st.total());
    std::printf("  port I/O  in/out : %" PRIu64 " / %" PRIu64 "\n", st.pio_in, st.pio_out);
    std::printf("  MMIO read/write  : %" PRIu64 " / %" PRIu64 "\n", st.mmio_read, st.mmio_write);
 
    auto print_bus = [](const char* label, const Bus& bus) {
        for (const auto& [base, r] : bus.regions())
            std::printf("  %s %-10s @ 0x%" PRIx64 ": %" PRIu64 " reads, %" PRIu64 " writes\n", label,
                        r.dev->name(), base, r.reads, r.writes);
        if (bus.unmapped_reads() || bus.unmapped_writes())
            std::printf("  %s unmapped          : %" PRIu64 " reads, %" PRIu64 " writes\n", label,
                        bus.unmapped_reads(), bus.unmapped_writes());
    };
    print_bus("pio ", m.pio);
    print_bus("mmio", m.mmio);
 
    if (st.total() > 0) {
        const double us = std::chrono::duration<double, std::micro>(elapsed).count();
        std::printf("elapsed %.0f us  =>  ~%.1f us per exit (nested virtualization and per-byte flush inflate this)\n",
                    us, us / static_cast<double>(st.total()));
    }
}
 
// The payoff of theme A: follow several guest-virtual addresses all the way down.
void show_translations(Vcpu& vcpu, const GuestMemory& ram, uint64_t cr3) {
    struct Probe { uint64_t gva; const char* what; };
    const Probe probes[] = {
        {0x1000,       "guest code, identity-mapped"},
        {kDataGpa,     "data page, via its identity mapping"},
        {kAliasGva,    "data page, via the alias"},
        {kHoleGva,     "alias neighbour: its page-table entry is empty"},
        {kFarHoleGva,  "far hole: even the page-directory entry is empty"},
        {kMmioGpa,     "MMIO device: one 2 MiB uncached page"},
    };
 
    std::printf("\n--- address translation after the run (guest CR3 = 0x%" PRIx64 ") ---\n", cr3);
    for (const Probe& p : probes) {
        const Walk w = walk(ram, cr3, p.gva);
        const std::optional<uint64_t> k = vcpu.translate(p.gva);
 
        std::printf("\nGVA 0x%" PRIx64 "   (%s)\n", p.gva, p.what);
        if (w.present) std::printf("  our software walk : GPA 0x%" PRIx64 "%s\n", w.gpa, w.writable ? "" : " (read-only)");
        else           std::printf("  our software walk : not present (entry missing at level %d)\n", w.fault_level);
        if (k)         std::printf("  KVM_TRANSLATE     : GPA 0x%" PRIx64 "\n", *k);
        else           std::printf("  KVM_TRANSLATE     : invalid\n");
        if (w.present != k.has_value() || (w.present && w.gpa != *k))
            std::printf("  !! the two walks DISAGREE\n");
 
        if (!w.present) continue;
        if (w.gpa < ram.size()) {
            const uint8_t* hva = ram.host_ptr(w.gpa);
            std::printf("  memslot says      : HVA %p (our mmap)\n", static_cast<const void*>(hva));
            if (auto hpa = host_physical(hva)) std::printf("  host kernel says  : HPA 0x%" PRIx64 "\n", *hpa);
            else std::printf("  host kernel says  : HPA hidden (run as root to see page frame numbers)\n");
        } else {
            std::printf("  no memslot covers this GPA: touching it exits to us as MMIO\n");
        }
    }
}
 
}  // namespace
 
int main(int argc, char** argv) {
    try {
        const std::string guest_path = argc > 1 ? argv[1] : "guest.bin";
        const auto timeout = std::chrono::milliseconds(argc > 2 ? std::stoi(argv[2]) : 5000);
 
        install_kick_handler();
 
        Fd kvm(check(open("/dev/kvm", O_RDWR | O_CLOEXEC), "open /dev/kvm"));
        if (check(ioctl(kvm.get(), KVM_GET_API_VERSION, 0), "KVM_GET_API_VERSION") != 12)
            throw std::runtime_error("unexpected KVM API version (expected 12)");
        Fd vm(check(ioctl(kvm.get(), KVM_CREATE_VM, 0UL), "KVM_CREATE_VM"));
 
        // Guest RAM: the one memslot. (Memslot = "this GPA range is backed by this HVA range".)
        GuestMemory ram(kGuestRamSize);
        kvm_userspace_memory_region region{};
        region.slot = 0;
        region.guest_phys_addr = 0;
        region.memory_size = ram.size();
        region.userspace_addr = reinterpret_cast<uint64_t>(ram.host_ptr(0));
        check(ioctl(vm.get(), KVM_SET_USER_MEMORY_REGION, &region), "KVM_SET_USER_MEMORY_REGION");
 
        // Machine: a UART on the port bus, the control device on the MMIO bus.
        Machine machine;
        Uart16550 uart(std::cout);
        ControlDevice control([&](uint32_t code) {
            machine.powered_off = true;   // runs on the vCPU thread, which is also the only reader
            machine.exit_code = code;
        });
        machine.pio.attach(kUartPort, 8, uart);
        machine.mmio.attach(kMmioGpa, ControlDevice::kSize, control);
 
        // Load the guest and build its page tables, all BEFORE the vCPU thread exists, so the
        // thread-creation guarantee of lesson 1 makes every one of these writes visible to it.
        const std::vector<uint8_t> image = read_file(guest_path);
        ram.write(kGuestEntry, image.data(), image.size());
        const uint64_t cr3 = build_guest_tables(ram);
 
        Vcpu vcpu(vm.get(), kvm.get(), 0);
        vcpu.reset_to_long_mode(kGuestEntry, cr3);
 
        // ---- supervisor/worker split ----
        Shared shared;
        const auto t0 = std::chrono::steady_clock::now();
        std::thread vcpu_thread(vcpu_thread_main, std::ref(vcpu), std::ref(machine), std::ref(shared));
        // std::ref: std::thread COPIES its arguments into the new thread by default. We want
        // the thread to use our objects, not copies (Vcpu cannot be copied anyway).
 
        bool by_watchdog = false;
        {
            std::unique_lock<std::mutex> lk(shared.mu);
            // Sleeps until `finished` is true OR the timeout elapses. Returns the predicate's value.
            const bool finished = shared.cv.wait_for(lk, timeout, [&] { return shared.finished; });
            lk.unlock();
            if (!finished) {
                by_watchdog = true;
                std::fprintf(stderr, "watchdog: no result after %lld ms; the guest has sent %" PRIu64
                             " UART bytes so far. Kicking the vCPU thread.\n",
                             static_cast<long long>(timeout.count()), uart.tx_bytes());  // uart locks internally
                vcpu.request_stop();                                   // 1. set the flags (atomics)
                pthread_kill(vcpu_thread.native_handle(), SIGUSR1);    // 2. interrupt a blocked KVM_RUN
            }
        }
        vcpu_thread.join();   // lesson 1: wait for it AND get visibility of everything it wrote
        const auto elapsed = std::chrono::steady_clock::now() - t0;
 
        if (shared.error) std::rethrow_exception(shared.error);  // lesson 5: re-raise on this thread
 
        report(machine, shared.stats, std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed), by_watchdog);
        if (!by_watchdog) show_translations(vcpu, ram, cr3);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "tinyvm: %s\n", e.what());
        return 1;
    }
	
}










