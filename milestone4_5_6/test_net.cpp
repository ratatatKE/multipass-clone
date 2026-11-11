// test_net.cpp - verification without a hypervisor.
//
//   make test    plain run          make tsan    under ThreadSanitizer (race detector)
//   make asan    under AddressSanitizer + UBSan (memory errors, undefined behaviour)
//
// THE TRICK ("driver in the loop"): the guest drivers in guest/*.h are templates over a Hal
// (hardware access layer). Here we give them a SimHal whose "MMIO" calls go straight into the
// host's Bus, and whose "guest-physical addresses" index a test RAM buffer. The real driver code
// then talks to the real device model, in one process, with no KVM, no root, no TAP.
// What this proves: driver and device agree with EACH OTHER, the ring protocol works, the
// device refuses hostile input, and the threads do not race.
// What it cannot prove: that either side matches the virtio SPECIFICATION (both were written by
// the same author) or that the KVM glue works. For the spec, compare abi.h with Linux's
// drivers/virtio/virtio_mmio.c; for KVM, run ./tinyvm.

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "abi.h"
#include "device.h"
#include "dma.h"
#include "guest/console.h"
#include "guest/ping_demo.h"
#include "guest/toy_driver.h"
#include "guest/virtio_driver.h"
#include "kvm_util.h"
#include "net_backend.h"
#include "netproto.h"
#include "toy_nic.h"
#include "virtio_net.h"

using namespace tinyvm;

static int failures = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            ++failures;                                                              \
        }                                                                            \
    } while (0)

// ---- the simulated hardware layer ---------------------------------------------------------
struct SimHal {
    static inline GuestMemory* ram = nullptr;
    static inline Bus* bus = nullptr;
    static inline std::string* console = nullptr;

    static uint32_t read32(uint64_t a) { uint32_t v = 0; bus->read(a, reinterpret_cast<uint8_t*>(&v), 4); return v; }
    static void write32(uint64_t a, uint32_t v) { bus->write(a, reinterpret_cast<const uint8_t*>(&v), 4); }
    static void* ptr(uint64_t gpa) { return ram->host_ptr(gpa); }
    static uint64_t gpa(const void* p) { return static_cast<const uint8_t*>(p) - ram->host_ptr(0); }
    // 1 tick = 1 nanosecond, so BootInfo::tsc_khz = 1,000,000 (ticks per millisecond).
    static uint64_t ticks() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    static void relax() { std::this_thread::yield(); }
    static void put_char(char c) { if (console) console->push_back(c); }
};

static const std::array<uint8_t, 6> kGuestMac = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static const std::array<uint8_t, 6> kGwMac = {0x52, 0x54, 0x00, 0x12, 0x35, 0x02};
constexpr uint32_t kGuestIp = 0x0a00020f, kGwIp = 0x0a000201;

static abi::BootInfo make_bootinfo(uint32_t ping_ip, uint32_t count) {
    abi::BootInfo bi{};
    bi.magic = abi::kBootMagic;
    bi.tsc_khz = 1000000;
    std::copy(kGuestMac.begin(), kGuestMac.end(), bi.guest_mac);
    bi.guest_ip = kGuestIp;
    bi.gateway_ip = kGwIp;
    bi.ping_ip = ping_ip;
    bi.ping_count = count;
    return bi;
}

static bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

// ---- netproto -------------------------------------------------------------------------------
static void test_checksum_and_builders() {
    // RFC 1071 section 3 example: the words 0001 f203 f4f5 f6f7 sum to 0xddf2 after folding.
    const uint8_t rfc[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    CHECK(net::inet_checksum(rfc, sizeof rfc) == 0x220d);

    uint8_t f[net::kMaxFrame];
    const size_t n = net::build_icmp_echo(f, kGuestMac.data(), kGwMac.data(), kGuestIp, 0x01010101, 0x7456, 9, 32);
    CHECK(n == 14 + 20 + 8 + 32);
    // A valid packet checksums to zero when the checksum field is included.
    CHECK(net::inet_checksum(f + 14, 20) == 0);
    CHECK(net::inet_checksum(f + 14 + 20, 8 + 32) == 0);

    uint8_t r[net::kMaxFrame];
    const size_t rn = net::make_echo_reply(f, n, r);
    CHECK(rn == n);
    const net::Ipv4Hdr* ip = nullptr;
    const net::IcmpEcho* e = net::as_icmp_echo(r, rn, &ip);
    CHECK(e && e->type == net::kIcmpEchoReply);
    CHECK(net::bswap16(e->seq) == 9 && net::bswap16(e->id) == 0x7456);
    CHECK(net::get_ip(ip->src) == 0x01010101 && net::get_ip(ip->dst) == kGuestIp);
    CHECK(net::inet_checksum(r + 14, 20) == 0 && net::inet_checksum(r + 34, rn - 34) == 0);
    CHECK(net::make_echo_reply(r, rn, f) == 0);  // a reply is not a request
}

// ---- GatewayBackend in isolation -----------------------------------------------------------
static void test_gateway_backend() {
    GatewayBackend gw(kGwIp, kGwMac);
    uint8_t f[net::kMaxFrame];

    size_t n = net::build_arp_request(f, kGuestMac.data(), kGuestIp, kGwIp);
    auto replies = gw.respond(std::vector<uint8_t>(f, f + n));
    CHECK(replies.size() == 1);
    const net::ArpPkt* a = net::as_arp(replies[0].data(), replies[0].size());
    CHECK(a && net::bswap16(a->oper) == net::kArpReply && net::beq(a->sha, kGwMac.data(), 6));

    n = net::build_arp_request(f, kGuestMac.data(), kGuestIp, 0x0a000263);  // someone else's IP
    CHECK(gw.respond(std::vector<uint8_t>(f, f + n)).empty());

    n = net::build_icmp_echo(f, kGuestMac.data(), kGwMac.data(), kGuestIp, 0x08080808, 1, 1, 16);
    CHECK(gw.respond(std::vector<uint8_t>(f, f + n)).size() == 1);   // pretend internet: any IP answers
    static const uint8_t other_mac[6] = {2, 0, 0, 0, 0, 9};
    n = net::build_icmp_echo(f, kGuestMac.data(), other_mac, kGuestIp, 0x08080808, 1, 1, 16);
    CHECK(gw.respond(std::vector<uint8_t>(f, f + n)).empty());      // not addressed to the gateway's MAC

    // Asynchronous delivery through the worker thread.
    std::mutex m;
    std::condition_variable cv;
    int got = 0;
    gw.set_rx_callback([&](const uint8_t*, size_t) {
        { std::lock_guard<std::mutex> lk(m); ++got; }
        cv.notify_all();
    });
    gw.start();
    n = net::build_arp_request(f, kGuestMac.data(), kGuestIp, kGwIp);
    gw.send(f, n);
    {
        std::unique_lock<std::mutex> lk(m);
        CHECK(cv.wait_for(lk, std::chrono::seconds(5), [&] { return got == 1; }));
    }
    gw.stop();
    gw.stop();  // idempotent
}

// ---- the full stack: real guest driver -> device model -> gateway back-end -----------------------
static void test_toy_end_to_end() {
    GuestMemory ram(1 << 20);
    Bus bus;
    GatewayBackend gw(kGwIp, kGwMac);
    ToyNic nic(gw, kGuestMac);
    bus.attach(abi::kToyNicBase, abi::kMmioWindow, nic);
    gw.start();

    std::string out;
    SimHal::ram = &ram; SimHal::bus = &bus; SimHal::console = &out;

    ToyDriver<SimHal> drv;
    CHECK(drv.init(abi::kToyNicBase));
    CHECK(net::beq(drv.mac(), kGuestMac.data(), 6));
    const abi::BootInfo bi = make_bootinfo(0x01010101, 3);
    PingDemo<SimHal, ToyDriver<SimHal>> demo(drv, bi);
    CHECK(demo.run() == 3);
    gw.stop();

    CHECK(contains(out, "ARP: 10.0.2.1 is at 52:54:00:12:35:02"));
    CHECK(contains(out, "reply from 1.1.1.1: seq=1"));
    CHECK(contains(out, "reply from 1.1.1.1: seq=3"));
    CHECK(contains(out, "3/3 replies"));
    const auto s = nic.stats();
    CHECK(s.tx_frames == 4 && s.rx_frames == 4);  // 1 ARP + 3 pings out; 1 ARP reply + 3 echo replies in
    const auto& region = bus.regions().at(abi::kToyNicBase);
    std::printf("  toy   : %llu register accesses for %llu frames\n",
                (unsigned long long)(region.reads + region.writes), (unsigned long long)(s.tx_frames + s.rx_frames));
}

static void test_virtio_end_to_end() {
    GuestMemory ram(1 << 20);
    GuestDma dma(ram);
    Bus bus;
    GatewayBackend gw(kGwIp, kGwMac);
    VirtioNetMmio nic(dma, gw, kGuestMac);
    bus.attach(abi::kVirtioNetBase, abi::kMmioWindow, nic);
    gw.start();

    std::string out;
    SimHal::ram = &ram; SimHal::bus = &bus; SimHal::console = &out;

    VirtioNetDriver<SimHal> drv;
    CHECK(drv.init(abi::kVirtioNetBase));
    CHECK(net::beq(drv.mac(), kGuestMac.data(), 6));
    CHECK(nic.status() == 0xf);  // ACK | DRIVER | FEATURES_OK | DRIVER_OK
    const abi::BootInfo bi = make_bootinfo(0x01010101, 3);
    PingDemo<SimHal, VirtioNetDriver<SimHal>> demo(drv, bi);
    CHECK(demo.run() == 3);
    gw.stop();

    CHECK(contains(out, "ARP: 10.0.2.1 is at 52:54:00:12:35:02"));
    CHECK(contains(out, "3/3 replies"));
    const auto s = nic.stats();
    CHECK(s.tx_frames == 4 && s.rx_frames == 4);
    CHECK(s.bad_descriptors == 0 && s.rx_dropped_no_buffer == 0 && s.feature_rejections == 0);
    const auto& region = bus.regions().at(abi::kVirtioNetBase);
    std::printf("  virtio: %llu register accesses for %llu frames\n",
                (unsigned long long)(region.reads + region.writes), (unsigned long long)(s.tx_frames + s.rx_frames));
}

// A back-end for tests: it records what the guest sends and lets the test inject frames "from the wire".
struct InjectBackend : NetBackend {
    const char* name() const override { return "inject"; }
    void send(const uint8_t* f, size_t n) override { sent_frames.emplace_back(f, f + n); ++sent_; }
    void start() override {}
    void stop() override {}
    void inject(const std::vector<uint8_t>& f) { if (rx_cb_) rx_cb_(f.data(), f.size()); }
    std::vector<std::vector<uint8_t>> sent_frames;
};

// More frames arrive than the guest posted empty buffers for: the extra ones are dropped and
// COUNTED (a real NIC does the same), and a buffer the guest hands back is reused.
static void test_virtio_rx_buffer_exhaustion() {
    GuestMemory ram(1 << 20);
    GuestDma dma(ram);
    Bus bus;
    InjectBackend wire;
    VirtioNetMmio nic(dma, wire, kGuestMac);
    bus.attach(abi::kVirtioNetBase, abi::kMmioWindow, nic);
    std::string out;
    SimHal::ram = &ram; SimHal::bus = &bus; SimHal::console = &out;

    VirtioNetDriver<SimHal> drv;
    CHECK(drv.init(abi::kVirtioNetBase));

    auto frame_with = [](uint8_t tag) {
        std::vector<uint8_t> f(60, tag);
        return f;
    };
    for (uint8_t i = 0; i < 6; ++i) wire.inject(frame_with(i));  // driver posted 4 buffers
    CHECK(nic.stats().rx_frames == 4);
    CHECK(nic.stats().rx_dropped_no_buffer == 2);

    uint8_t buf[net::kMaxFrame];
    for (uint8_t i = 0; i < 4; ++i) {  // frames come out in order, intact
        CHECK(drv.recv(buf, sizeof buf, 1'000'000'000ull) == 60);
        CHECK(buf[0] == i && buf[59] == i);
    }
    wire.inject(frame_with(9));  // buffers were returned by recv(), so this fits again
    CHECK(nic.stats().rx_frames == 5);
    CHECK(drv.recv(buf, sizeof buf, 1'000'000'000ull) == 60 && buf[0] == 9);

    // Transmit side: the frame the driver sends arrives at the back-end byte for byte.
    std::vector<uint8_t> tx(100);
    for (size_t i = 0; i < tx.size(); ++i) tx[i] = static_cast<uint8_t>(i * 7);
    CHECK(drv.send(tx.data(), static_cast<uint32_t>(tx.size())));
    CHECK(wire.sent_frames.size() == 1 && wire.sent_frames[0] == tx);

    // Ring wrap-around: send far more frames than the ring has entries (8).
    for (int i = 0; i < 40; ++i) CHECK(drv.send(tx.data(), 64));
    CHECK(wire.sent_frames.size() == 41);
    CHECK(nic.stats().bad_descriptors == 0);
}

// ---- hostile guest -----------------------------------------------------------------------------
// A VMM must survive anything a guest writes into the rings. We drive the device by hand (no
// driver) and give it garbage.
static void write_reg(Bus& bus, uint64_t base, uint32_t reg, uint32_t v) {
    bus.write(base + reg, reinterpret_cast<const uint8_t*>(&v), 4);
}
static uint32_t read_reg(Bus& bus, uint64_t base, uint32_t reg) {
    uint32_t v = 0;
    bus.read(base + reg, reinterpret_cast<uint8_t*>(&v), 4);
    return v;
}

static void handshake(Bus& bus, uint64_t b, uint64_t desc, uint64_t avail, uint64_t used, uint32_t qnum) {
    namespace v = abi::virtio;
    write_reg(bus, b, v::kRegStatus, 0);
    write_reg(bus, b, v::kRegStatus, 3);
    write_reg(bus, b, v::kRegDriverFeaturesSel, 0);
    write_reg(bus, b, v::kRegDriverFeatures, 0);
    write_reg(bus, b, v::kRegDriverFeaturesSel, 1);
    write_reg(bus, b, v::kRegDriverFeatures, 1);
    write_reg(bus, b, v::kRegStatus, 0xb);
    for (uint32_t q = 0; q < 2; ++q) {
        write_reg(bus, b, v::kRegQueueSel, q);
        write_reg(bus, b, v::kRegQueueNum, qnum);
        write_reg(bus, b, v::kRegQueueDescLow, static_cast<uint32_t>(desc));
        write_reg(bus, b, v::kRegQueueDescHigh, static_cast<uint32_t>(desc >> 32));
        write_reg(bus, b, v::kRegQueueDriverLow, static_cast<uint32_t>(avail));
        write_reg(bus, b, v::kRegQueueDriverHigh, static_cast<uint32_t>(avail >> 32));
        write_reg(bus, b, v::kRegQueueDeviceLow, static_cast<uint32_t>(used));
        write_reg(bus, b, v::kRegQueueDeviceHigh, static_cast<uint32_t>(used >> 32));
        write_reg(bus, b, v::kRegQueueReady, 1);
    }
    write_reg(bus, b, v::kRegStatus, 0xf);
}

static void test_virtio_hostile_guest() {
    namespace v = abi::virtio;
    const uint64_t B = abi::kVirtioNetBase;

    // (a) feature negotiation: refusing VERSION_1 must be answered by clearing FEATURES_OK.
    {
        GuestMemory ram(1 << 20); GuestDma dma(ram); Bus bus; GatewayBackend gw(kGwIp, kGwMac);
        VirtioNetMmio nic(dma, gw, kGuestMac);
        bus.attach(B, abi::kMmioWindow, nic);
        write_reg(bus, B, v::kRegStatus, 3);
        write_reg(bus, B, v::kRegDriverFeaturesSel, 0);
        write_reg(bus, B, v::kRegDriverFeatures, 0);      // no VERSION_1 (bit 32)
        write_reg(bus, B, v::kRegStatus, 0xb);
        CHECK((read_reg(bus, B, v::kRegStatus) & v::kStatusFeaturesOk) == 0);
        CHECK(nic.stats().feature_rejections == 1);
        // ... and asking for a feature that was never offered is refused too.
        write_reg(bus, B, v::kRegStatus, 0);
        write_reg(bus, B, v::kRegDriverFeaturesSel, 1);
        write_reg(bus, B, v::kRegDriverFeatures, 1);
        write_reg(bus, B, v::kRegDriverFeaturesSel, 0);
        write_reg(bus, B, v::kRegDriverFeatures, 1u << 3);  // some bit we do not offer
        write_reg(bus, B, v::kRegStatus, 0xb);
        CHECK((read_reg(bus, B, v::kRegStatus) & v::kStatusFeaturesOk) == 0);
    }

    // (b) queue addresses outside RAM are refused at QueueReady time.
    {
        GuestMemory ram(1 << 20); GuestDma dma(ram); Bus bus; GatewayBackend gw(kGwIp, kGwMac);
        VirtioNetMmio nic(dma, gw, kGuestMac);
        bus.attach(B, abi::kMmioWindow, nic);
        handshake(bus, B, 0xfffffffffff0ull, 0x41000, 0x42000, 8);
        CHECK((nic.status() & v::kStatusNeedsReset) != 0);
    }

    // (c) a TX descriptor pointing outside RAM must not crash or leak: it is counted and returned.
    {
        GuestMemory ram(1 << 20); GuestDma dma(ram); Bus bus; GatewayBackend gw(kGwIp, kGwMac);
        VirtioNetMmio nic(dma, gw, kGuestMac);
        bus.attach(B, abi::kMmioWindow, nic);
        const uint64_t desc = 0x40000, avail = 0x41000, used = 0x42000;
        handshake(bus, B, desc, avail, used, 8);
        CHECK(nic.status() == 0xf);
        v::Desc d{0xffffffff00000000ull, 100, 0, 0};          // address far outside RAM
        std::memcpy(ram.host_ptr(desc), &d, sizeof d);
        uint16_t zero = 0, one = 1;
        std::memcpy(ram.host_ptr(avail + 4), &zero, 2);       // ring[0] = descriptor 0
        std::memcpy(ram.host_ptr(avail + 2), &one, 2);        // avail.idx = 1
        write_reg(bus, B, v::kRegQueueNotify, v::kTxQueue);
        CHECK(nic.stats().bad_descriptors == 1);
        CHECK(nic.stats().tx_frames == 0);
        uint16_t used_idx = 0;
        std::memcpy(&used_idx, ram.host_ptr(used + 2), 2);
        CHECK(used_idx == 1);                                  // buffer handed back anyway
    }

    // (d) a descriptor chain that loops (next -> itself) must terminate.
    {
        GuestMemory ram(1 << 20); GuestDma dma(ram); Bus bus; GatewayBackend gw(kGwIp, kGwMac);
        VirtioNetMmio nic(dma, gw, kGuestMac);
        bus.attach(B, abi::kMmioWindow, nic);
        const uint64_t desc = 0x40000, avail = 0x41000, used = 0x42000;
        handshake(bus, B, desc, avail, used, 8);
        v::Desc d{0x50000, 64, v::kDescNext, 0};               // next = itself
        std::memcpy(ram.host_ptr(desc), &d, sizeof d);
        uint16_t zero = 0, one = 1;
        std::memcpy(ram.host_ptr(avail + 4), &zero, 2);
        std::memcpy(ram.host_ptr(avail + 2), &one, 2);
        write_reg(bus, B, v::kRegQueueNotify, v::kTxQueue);    // would hang forever without loop detection
        CHECK(nic.stats().bad_descriptors == 1);
    }

    // (e) a TX chain whose first descriptor is device-WRITABLE (wrong direction) is refused.
    {
        GuestMemory ram(1 << 20); GuestDma dma(ram); Bus bus; GatewayBackend gw(kGwIp, kGwMac);
        VirtioNetMmio nic(dma, gw, kGuestMac);
        bus.attach(B, abi::kMmioWindow, nic);
        const uint64_t desc = 0x40000, avail = 0x41000, used = 0x42000;
        handshake(bus, B, desc, avail, used, 8);
        v::Desc d{0x50000, 64, v::kDescWrite, 0};
        std::memcpy(ram.host_ptr(desc), &d, sizeof d);
        uint16_t zero = 0, one = 1;
        std::memcpy(ram.host_ptr(avail + 4), &zero, 2);
        std::memcpy(ram.host_ptr(avail + 2), &one, 2);
        write_reg(bus, B, v::kRegQueueNotify, v::kTxQueue);
        CHECK(nic.stats().bad_descriptors == 1);
    }

    // (f) bogus queue sizes are ignored; a doorbell before DRIVER_OK does nothing.
    {
        GuestMemory ram(1 << 20); GuestDma dma(ram); Bus bus; GatewayBackend gw(kGwIp, kGwMac);
        VirtioNetMmio nic(dma, gw, kGuestMac);
        bus.attach(B, abi::kMmioWindow, nic);
        write_reg(bus, B, v::kRegQueueSel, 0);
        write_reg(bus, B, v::kRegQueueNum, 7);                 // not a power of two
        write_reg(bus, B, v::kRegQueueReady, 1);               // num is still 0 -> refused
        CHECK((nic.status() & v::kStatusNeedsReset) != 0);
        write_reg(bus, B, v::kRegQueueSel, 5);                 // no such queue
        CHECK(read_reg(bus, B, v::kRegQueueNumMax) == 0);
        write_reg(bus, B, v::kRegQueueNotify, v::kTxQueue);    // not DRIVER_OK: ignored
        CHECK(nic.stats().tx_frames == 0);
    }
}

// Probing an absent device must fail cleanly (open bus reads all-ones).
static void test_probe_absent_device() {
    GuestMemory ram(1 << 20);
    Bus bus;  // no devices at all
    std::string out;
    SimHal::ram = &ram; SimHal::bus = &bus; SimHal::console = &out;
    VirtioNetDriver<SimHal> v;
    ToyDriver<SimHal> t;
    CHECK(!v.init(abi::kVirtioNetBase));
    CHECK(!t.init(abi::kToyNicBase));
}

// Many frames, many threads: the toy NIC's RX queue is hit by the back-end thread while the
// "guest" thread drains it. Run under TSan to see the mutex doing its job.
static void test_toy_rx_concurrency() {
    GuestMemory ram(1 << 20);
    Bus bus;
    GatewayBackend gw(kGwIp, kGwMac);
    ToyNic nic(gw, kGuestMac);
    bus.attach(abi::kToyNicBase, abi::kMmioWindow, nic);
    gw.start();
    std::string out;
    SimHal::ram = &ram; SimHal::bus = &bus; SimHal::console = &out;

    ToyDriver<SimHal> drv;
    CHECK(drv.init(abi::kToyNicBase));
    uint8_t f[net::kMaxFrame], buf[net::kMaxFrame];
    int received = 0;
    for (int i = 0; i < 50; ++i) {
        const size_t n = net::build_arp_request(f, kGuestMac.data(), kGuestIp, kGwIp);
        drv.send(f, static_cast<uint32_t>(n));
        if (drv.recv(buf, sizeof buf, 2'000'000'000ull) > 0) ++received;  // up to 2 s per reply
    }
    gw.stop();
    CHECK(received == 50);
}

int main() {
    test_checksum_and_builders();
    test_gateway_backend();
    test_toy_end_to_end();
    test_virtio_end_to_end();
    test_virtio_rx_buffer_exhaustion();
    test_virtio_hostile_guest();
    test_probe_absent_device();
    test_toy_rx_concurrency();

    if (failures == 0) std::printf("net tests passed\n");
    else std::printf("%d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}