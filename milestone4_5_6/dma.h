/*

    dma.h - how a DEVICE MODEL reaches into the guest's RAM. DMA = Direct Memory Access.

    Real NICs and disks read and write system RAM by themselves, without the CPU copying each byte. This enables the CPU to do other things as we saw in Linux 0.12.
    In a VMM, the "device" is our C__ code and "guest RAM" is the mmap()'ed buffer.

    Two rules make this read and write safe:
    1.  NEVER TRUST A GUEST ADDRESS. A descriptor entry in the descriptor table says "the frame is at guest-physical address X, length N".

        X and N come from guest memory, which a buggy or malicious guest controls. If we blindly did memcpy(host_ptr(X), ..., N), a guest could make us
        read or overwrite our own process memory causing a "VM escape".
        To prevent this, we have to check the bounds i.e. N. Every access below is bounds-checked and written to be overflow-safe:
            - We test `gpa <= size - len` and never `gpa + len <=size` because the sum could wrap around 2 ^ 64 and pass the check.

    2.  NEVER READ THE SAME GUEST BYTES TWICE AND ASSUME THEY MATCH ("double fetch").
        While our code runs, the guest's vCPU may still be running and can change those bytes ant any moment.
        So copy the descriptor entry from the descriptor table into a LOCAL VARIABLE ONCE, validate the local copy and only use the LOCAL copy aftewards.
        Validating guest memory and then re-reading it is a classic TOCTOU (time-of-check/time-of-use) vulnerability.

*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "kvm_util.h"

namespace tinyvm
{

    class GuestDma
    {
    public:
        explicit GuestDma(GuestMemory &mem) : mem_(mem) {}

        bool valid(uint64_t gpa, uint64_t len) const { return len <= mem_.size() && gpa <= mem_.size() - len; }

        bool read(uint64_t gpa, void *dst, size_t len) const
        {
            if (!valid(gpa, len))
                return false;
            std::memcpy(dst, mem_.host_ptr(gpa), len);
            return true;
        }

        bool write(uint64_t gpa, const void *src, size_t len)
        {
            if (!valid(gpa, len))
                return false;
            std::memcpy(mem_.host_ptr(gpa), src, len);
            return true;
        }

        /*
            Publication primitives (see "threading lesson 7" in virtio_net.h)
            A virtque index (avail.idx / used.idx) is how one side tells the other "I have finished writing entries up to here".
            It must be read with ACQUIRE and written with RELEASE semantics, otherwise the reader could see the new index but STALE entry contents.
        */
        bool load_acquire_u16(uint64_t gpa, uint16_t &out) const
        {
            if (!valid(gpa, 2) || (gpa & 1))
                return false;
            out = __atomic_load_n(reinterpret_cast<uint16_t *>(mem_.host_ptr(gpa)), __ATOMIC_ACQUIRE);
            return true;
        }

        bool store_release_u16(uint64_t gpa, uint16_t val)
        {
            if (!valid(gpa, 2) || (gpa & 1))
                return false;
            __atomic_store_n(reinterpret_cast<uint16_t *>(mem_.host_ptr(gpa)), val, __ATOMIC_RELEASE);
            return true;
        }

    private:
        GuestMemory &mem_;
    };

} // namespace tinyvm
