// This changed because for milestone 3 we wanted to learn about multithreading as well. device.h, kvm_util.h and test_bus.cpp remained the same from milestone2
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <ostream>
#include <utility>

#include "device."

namespace tinyvm
{

    /*
    ---------------------------------------------------------------------------------------
        16550 UART (subset). Eight byte-wide registers selected by offset 0..7:
        0  THR (write) / RBR (read) / DLL (divisor latch low,  when LCR.DLAB=1)
        1  IER                      / DLM (divisor latch high, when LCR.DLAB=1)
        2  IIR (read) / FCR (write)
        3  LCR  (bit 7 = DLAB)
        4  MCR
        5  LSR  (bit 5 = THR empty, bit 6 = transmitter idle, bit 0 = data ready)
        6  MSR
        7  scratch
        We transmit instantly (LSR always says "ready") and never have received data. No IRQs yet.

    --------- WHY THIS CLASS NOW HAS A MUTEX (threading lesson 3 of 5) --------------------
    In milestone 2 exactly one thread ever called this Uart16550 device, so nothing could have gone wront.
    A real VMM is different: each virtual CPU is its own thread, and then there are I/O threads
    (network, disk, console input). Any of them may reach this same Uart16550 device at the same moment.

    Two threads inside the write() function of this device at the same time would for example do `++tx_byte_`. At machine
    level, that is three steps i.e. `load tx_byte_` into a register, add 1, store it back. If a core A
    and core B both load 7 before either stores, both store 8 at the same time and one 1 has vanished from
    the count. Worse, two threads writing into the same std::ostream at once corrupts the
    stream's internal buffer pointers - that's not a "lost update", it is a crash waiting
    to happen. The C++ standard calls an unsynchronised conflicting access a "data race" and
    give it NO defined behaviour at all.

    The fix is a mutual-exclusion lock(mutex): a word in memory that is 0 when free and
    non-zero when some thread owns it. `mu_.lock()` means "wait until I can flip it from 0
    to 1 as one indivisible step". On x86 that indivisible step is a `lock cmpxchg` instruction:
    the `lock` prefix makes this core take exclusive ownership of the cache line holding the
    word, so no other core can touch it mid-operation. If the lock was already taken, the
    thread asks the kernel to put it to sleep (futex system call) rather than burning CPU.
    If the lock was free, the whole thign costs about one locked instruction and NO system
    call. This is why mutexes are cheap when uncontended.

    `std::lock_guard<std::mutex> lk(mu_);` is the C++ idiom for "lock now, and unlock
    automatically when this scope ends". It exists for the same reason you write
    `goto cleanup:` in C - There are many ways out of a function (return, early return,
    exxception) and forgetting the unlock on just one of them leaves the lock held forever,
    so the next thread to call lock() sleeps forever (a deadlock). The guard's destroyer runs on
    every path out of the scope so the unlock cannot be forgotten.

    `mutable` on the mutex lets const member functions (like tx_bytes const) still lock it.
    Since the mutable variable finds itself in a const context.
    Locking changes the mutex's bytes but not the logical state of the UART. I undertand this
    from SerenityOS

    */

    class Uart16550 : public Device
    {

    public:
        explicit Uart16550(std::ostream &out) : out_(out) {}

        const char *name() const override{return "uart16550"}

        void write(uint64_t offset, const uint8_t *data, uint32_t) override
        {
            std::lock_guard<std::mutex> lk(mu_); // everything below is critical section.

            const uint8_t v = data[0];
            switch (offset)
            {
            case 0:
                if (dlab())
                {
                    dll_ = v;
                }
                else
                {
                    out_.put(static_cast<char>(v));
                    out_.flush(); // flush the std::ostream, this outputs it to stream immediately (ONE byte at a time?)
                    ++tx_bytes;
                }

            case 1:
                if(dlab())
            }
        }
    }

} // namespace tinyvm
