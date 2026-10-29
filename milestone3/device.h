/*
    device.h - the device model: a Device and the Bus that routes access to it.

    A guest talks to hardware in exxactly two ways on x86:
        * port I/O  (in/out instructions, 16-bit port numbers) -> KVM_EXIT_IO
        * MMIO      (ordinary loads/stores to a physical address that has no RAM behind it is MMIO access) -> KVM_EXIT_MMIO

    Either way the CPU cannot complete the instructions by itself, so it exits to the hypervisor,
    which looks up "who owns this address?" and calls the device. That lookup is the Bus.
    We use one Bus class for both address spaces.

*/

#pragma once

#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <stdexcept>

namespace tinyvm
{

    class Device
    {
    public:
        virtual ~Device() = default;
        virtual const char *name() const = 0;

        // `offset` is relative to the base address the device was attached at.
        // `len` is the access width in bytes (1, 2, 4, 8)
        // NdedaC: Why are we initializing these functions to 0? Aah, is see this function name is overriden in devices.h
        virtual void read(uint64_t offset, uint8_t *data, uint32_t len) = 0;
        virtual void write(uint64_t offset, const uint8_t *data, uint32_t len) = 0;
    };

    class Bus
    {
    public:
        struct Region
        {
            uint64_t base = 0;
            uint64_t size = 0;
            Device *dev = nullptr;
            uint64_t reads = 0;
            uint64_t writes = 0;
        };

        /*
            Attach a device at {base, base+size}. Overlapping regions are a BUG IN THE MACHINE definition, so we refuse them
            loudly instead of letting one device shadow another
        */

        void attach(uint64_t base, uint64_t size, Device &dev)
        {
            if (size == 0)
                throw std::invalid_argument("bus: empty region");

            const uint64_t end = base + size;
            auto next = regions_.lower_bound(base);

            if (next != regions_.end() && next->first < end) // next of region should be greater than end of devices
                throw std::invalid_argument("bus: region overlaps an existing device");

            if (next != regions_.begin())
            {
                auto prev = std::prev(next);
                if (prev->second.base + prev->second.size > base)
                    throw std::invalid_argument("bus : region overlaps and existing device");
            }

            regions_[base] = Region{base, size, &dev};
        }

        /*
            Returns true if a device handled the access.
            Unmapped reads return all-ones, like a real bus with nothing driving the data lines.
            Unmapped writes are dropped
        */
        bool read(uint64_t addr, uint8_t *data, uint32_t len)
        {
            if (Region *r = find(addr))
            {
                ++r->reads;
                r->dev->read(addr - r->base, data, len);
                return true;
            }

            std::memset(data, 0xff, len);
            ++unmapped_reads_;
            return false;
        }

        bool write(uint64_t addr, const uint8_t *data, uint32_t len)
        {
            if (Region *r = find(addr))
            {
                ++r->writes;
                r->dev->write(addr - r->base, data, len);
                return true;
            }

            ++unmapped_writes_;
            return false;
        }

        const std::map<uint64_t, Region> &regions() const { return regions_; }
        uint64_t unmapped_reads() const { return unmapped_reads_; }
        uint64_t unmapped_writes() const { return unmapped_writes_; }

    private:
        /*
            regions are keyed by base address, so "the region containing addr" os the last region whose base <= addr, provided address is still inside it. O(log n)
        */

        Region *find(uint64_t addr)
        {
            auto it = regions_.upper_bound(addr);
            if (it == regions_.begin())
                return nullptr;
            --it;
            Region &r = it->second;
            return addr < r.base + r.size ? &r : nullptr;
        }

        std::map<uint64_t, Region> regions_;
        uint64_t unmapped_reads_ = 0;
        uint64_t unmapped_writes_ = 0;
    };

}; // namespace tinyvm