/*
    This file includes two example devices:
    - Uart16550   : Port I/O at 0x3f8..0x3ff, the classic PC serial port (subset).
    - ControlDevice:  MMIO at 0xd0000000, a made-up device that serves as a tutorial for
     MMIO read/write, device state and how a device can TALK BACK to the hypervisor.
*/

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <ostream>
#include <utility>

#include "device.h"

namespace tinyvm
{

    /*

        16550 UART(subset).
        Eight byte-wide registers selected by offset 0..7;

        0   THR(write) / RBR (read) / DLL (divisor latch low, when LCR.DLAB=1)
        1   IER                     / DLM (divisor latch high, when LCR.DLAB=1)
        2   IIR (read) /FCR(write)
        3   LCR (bit 7 = DLAB)
        4   MCR
        5   LSR (bit 5 = THR empty, bit 6= transmitter idle, bit 0 = data ready)
        6   MSR
        7   Scratch

        We transmit instantly (LSR register always says "ready") and have never received data since we no IRQs yet

    */
    class Uart16550 : public Device
    {
    public:
        explicit Uart16550(std::ostream &out) : out_(out) {}

        const char *name() const override{return "uart16550"; }

        void write(uint64_t off, const uint8_t *data, uint32_t) override
        { // let me see if it will work without specifying len as defined in parent. Oooh, I think len was left out because we aren't really using it in write(). Nice to learn something.
            const uint8_t v = data[0];

            switch (off)
            {
            case 0:
                if (dlab())
                {
                    dll_ = v;
                }
                else
                {
                    out_.put(static_cast<char>(v));
                    out_.flush();
                    ++tx_bytes_;
                }
                break;

            case 1:
                if (dlab())
                    dlm_ = v;
                else
                    ier_ = v & 0x0f;
                break;
            case 2:
                fcr_ = v;
                break;
            case 3:
                lcr_ = v;
                break;
            case 4:
                mcr_ = v & 0x1f;
                break;
            case 7:
                scr_ = v;
                break;
            default:
                break; // LSR and MSR are read-only;
            }
        }

        void read(uint64_t off, uint8_t *data, uint32_t len) override
        { // in read we use len so we pass it as variable.
            uint8_t v = 0;
            switch (off)
            {
            case 0:
                v = dlab() ? dll_ : 0;
                break; // nothing received
            case 1:
                v = dlab() ? dlm_ : ier_;
                break;
            case 2:
                v = 0x01;
                break; // IIR bit 0 set: no interrupt pendign
            case 3:
                v = lcr_;
                break;
            case 4:
                v = mcr_;
                break;
            case 5:
                v = 0x60;
                break; // THRE | TEMT: always ready to transmit
            case 6:
                v = 0x00;
                break;
            case 7:
                v = scr_;
                break;
            default:
                break;
            }

            std::memset(data, 0, len);
            data[0] = v;
        }

        uint64_t tx_bytes() const { return tx_bytes_; }

    private:
        bool dlab() const { return (lcr_ & 0x80) != 0; }

        std::ostream &out_;
        uint8_t ier_ = 0, fcr_ = 0, lcr_ = 0, mcr_ = 0, scr_ = 0, dll_ = 0, dlm_ = 0;
        uint64_t tx_bytes_ = 0;
    };

    /*
        ControlDevice: 32-bit registers at offsets
        0x00        ID                  read-only       magic 0x324d5654 ("TVM2" in little-endian bytes)
        0x04        ACCESS_COUNT        read-only       number of accesses (reads + writes) that this device has seen, including the current one: every one was a VM exit
        0x08        POWEROFF            write-only      guest writes an exit node; the hypervisor stops the VM
        0x0c        SCRATCH             read/write      plan storage, to show device state persists
    */

    class ControlDevice : public Device
    {
    public:
        static constexpr uint32_t kMagic = 0x324d5654;
        static constexpr uint64_t kSize = 0x1000;

        explicit ControlDevice(std::function<void(uint32_t)> on_poweroff) : on_poweroff_(std::move(on_poweroff)) {}

        const char *name() const override { return "control"; }

        void read(uint64_t off, uint8_t *data, uint32_t len) override
        {
            ++accesses_;
            uint32_t v = 0;
            switch (off)
            {
            case 0x00:
                v = kMagic;
                break;
            case 0x04:
                v = static_cast<uint32_t>(accesses_);
                break;
            case 0x0c:
                v = scratch_;
                break;
                ;
            default:
                break;
            }

            std::memset(data, 0, len);
            std::memcpy(data, &v, std::min<uint32_t>(len, 4)); // assumes little-endian host (x86)
        }

        void write(uint64_t off, const uint8_t *data, uint32_t len) override
        {
            ++accesses_;
            uint32_t v = 0;
            std::memcpy(&v, data, std::min<uint32_t>(len, 4));
            switch(off)
            {
            case 0x08:
                if (on_poweroff_)
                    on_poweroff_(v);
                break;
            case 0x0c:
                scratch_ = v;
                break;
            default:
                break;
            }
        }

        uint64_t accesses() const { return accesses_; }

    private:
        std::function<void(uint32_t)> on_poweroff_;
        uint64_t accesses_ = 0;
        uint32_t scratch_ = 0;
    };

} // namespace tinyvm
