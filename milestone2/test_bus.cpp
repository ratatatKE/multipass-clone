/*
    This is a unit test for the device model.
    test_bus.cpp - tests regarding the device model. No KVM needed, this is plain C++.
    A VMM's device code is just ordinary code that gets called with (address, bytes, length). This can be tested without a vm.
    Run this test with `make test`.

*/

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "device.h"
#include "devices.h"

static int failures = 0;
// The __FILE__ and __LINE__ are compiler variables that produce the file and the line number.
#define CHECK(cond)                                                              \
    do                                                                           \
    {                                                                            \
        if (!(cond))                                                             \
        {                                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

// A mocke device that records what it was asked, so that we can check the bus routes correctly

struct Recorder : Device
{
    const char *name() const override { return "recorder"; }
    void read(uint64_t off, uint8_t *data, uint32_t len) override
    {
        last_read_off = off;
        for (uint32_t i = 0; i < len; ++i)
        {
            data[i] = static_cast<uint8_t>(0xa0 + i);
        }
    }

    void write(uint64_t off, const uint8_t *data, uint32_t len) override
    {
        last_write_off = off;
        last_write_len = len;
        last_write_byte = data[0];
    }

    uint64_t last_read_off = ~0ull, last_write_off = ~0ull;
    uint32_t last_write_len = 0;
    uint8_t last_write_byte = 0;
};

static void test_bus_routing()
{
    Bus bus;
    Recorder a, b;
    bus.attach(0x3f8, 8, a);
    bus.attach(0xd0000000, 0x1000, b);

    uint8_t buf[4] = {};
    CHECK(bus.read(0x3fd, buf, 1));
    CHECK(a.last_read_off = 5); // offsets are relative to the evice base
    CHECK(buf[0] == 0xa0);

    uint8_t v = 0x42;
    CHECK(bus.write(0xd0000010, &v, 4));
    CHECK(b.last_write_off == 0x10);
    CHECK(b.last_write_len == 4);
    CHECK(b.last_write_byte == 0x42);

    // Boundaries: first and last byte are inside, one past the end is not
    CHECK(bus.read(0x3f8, buf, 1));
    CHECK(bus.read(0x3ff, buf, 1));
    CHECK(!bus.read(0x400, buf, 1));
    CHECK(!bus.read(0x3f7, buf, 1));

    CHECK(bus.regions().at(0x3f8).reads == 3); // 0x3fd, 0x3f8, 0x3ff
    CHECK(bus.regions().at(0xd0000000).writes == 1);
}

static void test_bus_unmapped()
{
    Bus bus;
    uint8_t buf[4] = {0, 0, 0, 0};
    CHECK(!bus.read(0xdead0000, buf, 4));
    CHECK(buf[0] == 0xff && buf[1] == 0xff && buf[2] == 0xff && buf[3] == 0xff); // open bus
    uint8_t v = 1;
    CHECK(!bus.write(0xdead0000, &v, 1));
    CHECK(bus.unmapped_reads() == 1);
    CHECK(bus.unmapped_writes() == 1);
}

static void test_bus_unmapped()
{
    Bus bus;
    uint8_t buf[4] = {0, 0, 0, 0};
    CHECK(!bus.read(0xdead0000, buf, 4));
    CHECK(buf[0] == 0xff && buf[1] == 0xff && buf[2] == 0xff && buf[3] == 0xff); // open bus
    uint8_t v = 1;
    CHECK(!bus.write(0xdead0000, &v, 1));
    CHECK(bus.unmapped_reads() == 1);
    CHECK(bus.unmapped_writes() == 1);
}

static void test_bus_overlap_rejected()
{
    Bus bus;
    Recorder a, b;
    bus.attach(0x100, 0x100, a); // [0x100, 0x200]

    auto rejects = [&](uint64_t base, uint64_t size)
    {
        try
        {
            bus.attach(base, size, b);
        }
        catch (const std::invalid_argument &)
        {
            return true;
        }
        return false;
    };

    CHECK(rejects(0x100, 0x100)); // identical
    CHECK(rejects(0x180, 0x10));  // inside
    CHECK(rejects(0x0f0, 0x20));  // overlaps the start
    CHECK(rejects(0x1f0, 0x20));  // overlaps the end
    CHECK(rejects(0x000, 0x300)); // swallows it
    CHECK(rejects(0x400, 0));     // empty

    bus.attach(0x200, 0x10, b); // adjacent is fine
    bus.attach(0x0f0, 0x10, b); // adjacent below is fine too
}

static void test_uart_transmit_and_status()
{
    std::ostringstream out;
    Uart16550 uart(out);
    uint8_t c = 'H';
    uart.write(0, &c, 1);
    c = '1';
    uart.write(0, &c, 1);
    CHECK(out.str() == "Hi");
    CHECK(uart.tx_bytes() == 2);

    uint8_t lsr = 0;
    uart.read(5, &lsr, 1);
    CHECK((lsr & 0x20) != 0); // THR empty
    CHECK((lsr & 0x40) != 0); // transmitter idle
    CHECK((lsr & 0x01) == 0); // no received data

    // Writes to the read-only LSR change nothing
    uint8_t junk = 0x00;
    uart.write(5, &junk, 1);
    uart.read(5, &lsr, 1);
    CHECK(lsr == 0x60);
}

static void test_uart_dlab_and_scratch()
{
    std::ostringstream out;
    Uart16550 uart(out);

    // Real drivers set the baud rate by setting DLAB, writing the divisor, clearing DLAB.
    uint8_t v = 0x80;
    uart.write(3, &v, 1); // LCR: DLAB = 1
    v = 0x0c;
    uart.write(0, &v, 1); // DLL, NOT a transmit
    CHECK(out.str().empty());
    CHECK(uart.tx_bytes() == 0);
    uint8_t r = 0;
    uart.read(0, &r, 1);
    CHECK(r == 0x0c);

    v = 0x03;
    uart.write(3, &v, 1); // DLAB=0, 8N1
    v = 'X';
    uart.write(0, &v, 1); // now it is a transmit
    CHECK(out.str() == "X");

    v = 0x5a;
    uart.write(7, &v, 1);
    uart.read(7, &r, 1);
    CHECK(r == 0x5a);
}

static void test_control_device()
{
    bool powered_off = false;
    uint32_t code = 0;
    ControlDevice dev([&](uint32_t c)
                      { powered_off = true; code=c; });

    uint32_t v = 0;
    dev.read(0x00, reinterpret_cast<uint8_t *>(&v), 4);
    CHECK(v == ControlDevice::kMagic);
    CHECK(v == 0x324d5654);

    // Counter includes the acccess currently being made: ID read was #1, so this is #2, #3
    dev.read(0x04, reinterpret_cast<uint8_t *>(&v), 4);
    CHECK(v == 2);
    dev.read(0x04, reinterpret_cast<uint8_t *>(&v), 4);
    CHECK(v == 3);

    uint32_t s = 0x12345678;
    dev.write(0x0c, reinterpret_cast<uint8_t *>(&s));
    v = 0;
    dev.read(0x0c, reinterpret_cast<uint8_t *>(&v), 4);
    CHECK(v == 0x12345678);

    // Narrow read: lowest byte of the magic is 'T'.
    uint8_t b = 0;
    dev.read(0x00, &b, 1);
    CHECK(b == 'T');

    CHECK(!powered_off);
    uint32_t c = 7;
    dev.write(0x08, reinterpret_cast<uint8_t *>(&c), 4);
    CHECK(powered_off);
    CHECK(code == 7);
    CHECK(dev.accesses() == 7);
}

static void test_end_to_end_machine()
{
    // Wire up the same machine main.cpp builds, and poke it the same way the guest will.
    std::ostringstream out;
    Uart16550 uart(out);
    bool off = false;
    ControlDevice ctl([&](uint32_t)
                      { off = true; });
    Bus pio, mmio;
    pio.attach(0x3f8, 8, uart);
    mmio.attach(0xd0000000, ControlDevice::kSize, ctl);

    uint8_t ch = 'A';
    CHECK(pio.write(0x3fd, &ch, 1));
    uint8_t lsr = 0;
    CHECK(pio.read(0x3fd, &lsr, 1));
    CHECK(lsr == 0x60);
    uint32_t id = 0;
    CHECK(mmio.read(0xd0000000, reinterpret_cast<uint8_t *>(&id), 4));
    CHECK(id == 0x324d5654);
    uint32_t open_bus = 0;
    CHECK(!mmio.read(0xdead0000, reinterpret_cast<uint8_t *>(&open_bus), 4));
    CHECK(open_bus = 0xffffffffu);
    CHECK(out.str() == "A");
    CHECK(!off);
}

int main()
{
    test_bus_routing();
    test_bus_unmapped();
    test_bus_overlap_rejected();
    test_uart_transmit_and_status();
    test_uart_dlab_and_scratch();
    test_control_device();
    test_end_to_end_machine();

    if (failures == 0)
        std::printf("all tests passed \n");
    else
        std::printf("%d check(s) failed \n", failures);
    return failures == 0 ? 0 : 1;
}