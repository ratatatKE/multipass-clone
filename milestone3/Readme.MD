# tinyvm milestone 3: long mode, page tables, and threads

```
make test            # device + paging tests, plain C++ (no KVM needed)
make tsan            # same tests under ThreadSanitizer (race detector)
make threads_demo && ./threads_demo      # threading lessons 1-2, no KVM needed
make                 # tinyvm, guest.bin (64-bit), spin.bin (infinite loop)
./tinyvm             # needs Linux + /dev/kvm; add sudo to also see host-physical addresses
./tinyvm spin.bin 2000    # watchdog demo: kicks the stuck vCPU thread after 2 s
```

## What changed from milestone 2

| File | Change |
|---|---|
| `paging.h` (new) | `PageTables` builder (4-level, 4 KiB and 2 MiB pages), `walk()` software page walker, `host_physical()` via `/proc/self/pagemap`, and the shared guest memory layout |
| `guest.S` | now 64-bit; shows CR3/CR0/EFER, an aliased page, and MMIO through a virtual address |
| `spin.S` (new) | guest that never exits, to exercise the watchdog |
| `devices.h` | `Uart16550` gets a `std::mutex` (threading lesson 3) |
| `main.cpp` | `reset_to_long_mode()`, vCPU runs on its own `std::thread`, watchdog with `condition_variable`, `KVM_TRANSLATE` report, `dump_state()` on fatal exits |
| `threads_demo.cpp`, `test_m3.cpp` (new) | threading demo; tests for paging and for UART thread safety |
| `device.h`, `kvm_util.h` | unchanged |

## Expected output of `./tinyvm`

Derived from the guest source, not captured from a real KVM run. CR0 and EFER may differ in a bit or two if KVM adjusts them.

```
tinyvm m3: guest is up (64-bit long mode, paging on)
CR3  (page table root)       : 0x0000000000010000
CR0  (PE/PG bits)            : 0x0000000080050033
EFER (LME=bit8, LMA=bit10)   : 0x0000000000000500
write VA 0x200000, read VA 0x20000 : 0x00000000feedface
write VA 0x20000, read VA 0x200000 : 0x000000000badf00d
MMIO via VA 0xd0000000, ID      : 0x00000000324d5654
MMIO access counter             : 0x0000000000000002
powering off with code 3
```

If that text matches, the exit statistics should read about 895 exits: 446 characters at two port exits each, plus 3 MMIO exits (two reads, one write). The `mov %cr3`, `rdmsr` and plain RAM accesses add none to the count that reaches us.

After the report, `show_translations()` prints, for six guest-virtual addresses, the guest-physical address found by our own walker and by `KVM_TRANSLATE` (they must agree), then the host-virtual address from the memslot and, as root, the host-physical page. The two "hole" addresses must be reported "not present" by both walkers.

## Three different "memory faults"; know them apart

| What the guest touches | Who notices | Result |
|---|---|---|
| A virtual address its own page tables don't map | The CPU (guest page walk, entry not present) | `#PF` delivered **to the guest**. With no IDT it escalates to a triple fault and you see `KVM_EXIT_SHUTDOWN` |
| A physical address inside a memslot, first touch | EPT/NPT (second-level walk) | KVM quietly fills in the mapping (a host page fault). Never reaches us |
| A physical address with **no memslot** | EPT/NPT | `KVM_EXIT_MMIO` to the VMM: this is how devices work |

## Threading lessons, and where to find each

| # | Lesson | Where | Try this |
|---|---|---|---|
| 1 | What a thread is; data races; `join()` | `main.cpp` primer, `threads_demo.cpp` | Run `threads_demo`, then `make disasm-demo` and find the load/add/store triple |
| 2 | `std::atomic`, memory ordering, `volatile` is not atomic | `main.cpp` primer, `Vcpu::request_stop()` | Look at `lock add` in the disassembly; time the three variants |
| 3 | `std::mutex`, `lock_guard`, RAII | `devices.h` (UART) | Delete the two `lock_guard` lines in `Uart16550::write`/`read`, run `make tsan` and watch it report the race |
| 4 | `condition_variable`, lost wakeups, spurious wakeups | `main.cpp` watchdog wait | Change the predicate to `[]{ return true; }` and see why the predicate matters |
| 5 | Exceptions across threads, signals, `EINTR`, `immediate_exit` | `vcpu_thread_main`, `request_stop()` | Run `./tinyvm spin.bin 2000`; then comment out the `immediate_exit` store and think about when it would hang |

## Exercises

1. Put a `std::shared_mutex` around `Bus::attach/read/write` (many readers, rare writer) and make the per-region counters `std::atomic<uint64_t>`. What would two vCPU threads need that one doesn't?
2. Make the guest read through the unmapped alias neighbour (VA 0x201000) and read the `dump_state()` output. What does `rip` point at?
3. Mark the data page read-only (drop `kPteWritable`) and write to it from the guest. With CR0.WP set, what happens, and why does it become a triple fault instead of a nice error?
4. In `threads_demo`, give each thread its own counter in a padded struct and sum at the end. Why is this faster than both the atomic and the mutex?