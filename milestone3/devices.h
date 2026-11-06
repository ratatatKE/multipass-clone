// This changed because for milestone 3 we wanted to learn about multithreading as well. device.h, kvm_util.h and test_bus.cpp remained the same from milestone2
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <ostream>
#include <utility>

#include "device.h"

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

        const char *name() const override{ return "uart16550"; }

        void write(uint64_t offset, const uint8_t *data, uint32_t) override
        {
            std::lock_guard<std::mutex> locker(mu_); // everything below is critical section.

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
                    ++tx_bytes_;
                }

                break;

            case 1: // v = the data we are writing
                if(dlab()) dlm_ = v; else ier_ = v & 0x0f; break; 
            case 2:
                fcr_ = v; break;
            case 3:
                lcr_ = v; break;
            case 4:
                mcr_ = v &0x1f; break;
            case 7:
                scr_ = v; break;
            default: break; // LSR and MSR are read only
                
            }
        }

        void read(uint64_t offset, uint8_t* data, uint32_t len) override {
            std::lock_guard<std::mutex> locker(mu_);
            uint8_t v = 0; // initialize the data to be read as 0;

            switch(offset){
                case 0: v = dlab() ? dll_ : 0; break;           // nothing received
                case 1: v = dlab() ? dlm_ : ier_; break;
                case 2: v = 0b00000001; break;                    // IIR bit 0 set: no interrupt pending
                case 3: v = lcr_; break;
                case 4: v = mcr_; break;
                case 5: v = 0b01100000; break;                    //THRE | TEMT: always ready to transmit
                case 6: v = 0x00; break;
                case 7: v = scr_; break;
                default: break; 
            }

            std::memset(data, 0, len);
            data[0] = v;
        }


        // Called from OTHER threads too (the watchdog in main.cpp reads it) and so it must lock
        uint64_t tx_bytes() const {
            std::lock_guard<std::mutex> locker(mu_);
            return tx_bytes_;
        }

private:

        bool dlab() const { return (lcr_ & 0b10000000) != 0; }  // caller must hold mu_

        mutable std::mutex mu_;
        std::ostream& out_;
        uint8_t ier_=0, fcr_=0, lcr_=0, mcr_=0, scr_=0, dll_=0, dlm_=0;
        uint64_t tx_bytes_ = 0;

    };



/*
    ControlDevicce: 32-bit registers at offsets
    0x00    ID              read-only       magic 0x324d5654 ("TVM2" in little-endian bytes)
    0x04    ACCESS_COUNT    read_only       number of accesses (reads + writes ) that this device has seen, including the current one: every one of those accesses was a VM exit
    0x08    POWEROFF        write-only      guest writes an exit code then the hypervisor stops the VM
    0x0c    SCRATCH         read/write      plain storage to show device state persists.

    ControlDevice is NOT locked. In this project, ony the vCPU thread ever touches the ControlDevice while the VM runs.
    The main thread only reads the ControlDevice after join(). A variable owned by exactly one thread at a time needs no lock at all! 
    That's the cheapest synchronization there is.
*/


class ControlDevice: public Device {
public:
    static constexpr uint32_t kMagic = 0x324d5654;
    static constexpr uint64_t kSize = 0x1000;

    explicit ControlDevice(std::function<void(uint32_t)> on_poweroff): on_poweroff_(std::move(on_poweroff)) {} // moves on_poweroff from main device

    const char* name() const override { return "control"; }

    void read(uint64_t offset, uint8_t* data, uint32_t len) override {
        ++accesses_;
        uint32_t v = 0;
        switch(offset){
            case 0x00: v=kMagic; break;
            case 0x04: v=static_cast<uint32_t>(accesses_); break;
            case 0x0c: v=scratch_; break;
            default: break;
        }

        std::memset(data, 0, len);
        std::memcpy(data, &v, std::min<uint32_t>(len, 4)); // assumes little-endian host i.e. x86
    }

    void write(uint64_t offset, const uint8_t* data, uint32_t len) override {
        ++accesses_;
        uint32_t v=0;
        std::memcpy(&v, data, std::min<uint32_t>(len, 4));
        switch(offset){
            case 0x08: if (on_poweroff_) on_poweroff_(v); break;
            case 0x0c: scratch_ = v; break;
            default: break;
        }
    }

    uint64_t accesses() const { return accesses_; }

    private:
        std::function<void(uint32_t)> on_poweroff_;
        uint64_t accesses_ = 0;
        uint32_t scratch_ = 0;

};

} // namespace tinyvm
