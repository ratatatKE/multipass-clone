/*
    This is milestone 4, 5 and 6 combined: A virtual NIC using two approaches on two backends.
    You can read the theory first here: https://docs.google.com/document/d/1spjL2e5QnK1T-1DIXNQL2b8lbGhtRsju6ke-pKiYlp4/edit?tab=t.10idsg7t3hrb

    FRONTEND (what the guest sees)                                          BACKEND (how the host moves frames)
    --nic=toy NIC i.e. a programmed-I/O NIC                                 --backend=gateway i.e. in-process fake network, no root
    --nic=virtio  i.e. virtio-net driver (default)                          --backend=tap:NAME i.e. a real Linux TAP interface

    Any front-end works with any backend. The guest probes for whichever NIC is present, does an ARP lookup of its gateway and pings then powers off.
    The report at the end compares how many VM exits the NIC costed. This becomes the practical difference between the toy NIC implementation and virtio.

    To build run:
    --------------
    make

    To run examples
    --------------------
    ./tinyvm                                                (virtio + fake network. No root needed)
    ./tinyvm --nic=toy                                      (run using the programmed I/O NIC)
    ./tinyvm --backend=tap:tvmtap0 --ping 10.0.2.1          (real TAP set up with lab/net-lab.sh first)


    The vCPU/thread machinery is in vcpu.h (moved from m3). What is new lives in:
    abi.h, netproto.h                                       The contract and the packet formats
    net_backend.h [M4]                                      TapBackend, GatewayBackend (threading lessons 8,9)
    toy_nic.h [M4]                                          front-end #1
    virtio_net.h [M5]                                       front-end #2                (threading lessons 6, 7)
    guest/                                                  the drivers and the ping demo that run inside the VM
    lab/net-lab.sh [M6]                                     the host networking lab: bridge, TAP, NAT

*/

#include <fcntl.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>

#include <array>
#include <chrono>
#include <cinttypes>
#include <stdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "abi.h"
#include "device.h"
#include "devices.h"
#include "dma.h"
#include "kvm_util.h"
#include "net_backend.h"
#include "paging.h"
#include "toy_nic.h"
#include "vcpu.h"
#include "virtio_net.h"

using namespace tinyvm;

static_assert(abi::kControlBase == kMmioGpa, "abi.h and paging.h must agree on where MMIO for virtio lives");

namespace
{

    constexpr uint64_t kGuestRamSize = 1 << 20;
    constexpr uint64_t kGuestEntry = 0x1000;
    constexpr uint64_t kUartPort = 0x3f8;

    // The machine's identity on the (virtual) network. QEMU's default addresses, by tradition
    constexpr uint32_t kGuestIp = 0x0a00020f;  // 10.0.2.15
    constexpr uint32_t kGateWayIP = 0xa000201; // 10.0.2.1
    const std::array<uint8_t, 6> kGuestMac = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    const std::array<uint8_t, 6> kGatewayMac = {0x52, 0x54, 0x00, 0x12, 0x35, 0x02};

    struct Options
    {
        std::string guest = "guest.bin";
        std::string nic = "virtio";      // toy | virtio
        std::string backend = "gateway"; // gateway | tap:NAME
        std::string ping = "10.0.2.1";
        uint32_t count = 3;
        int timeout_ms = 20000;
    };

    Options parse_args(int argc, char **argv)
    {
        Options o;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto val = [&](const char *key) -> const char *
            {
                const size_t n = std::strlen(key);
                return a.compare(0, n, key) == 0 ? a.c_str + n : nullptr;
            };
        }

        if (const char *v = val("--nic="))
            o.nic = v;
        else if (const char *v = val("--backend="))
            o.backend = v;

        else if (const char *v = val("--ping="))
            o.ping = v;

        else if (const char *v = val("--count="))
            o.count = static_cast<uint32_t>(std::stoul(v));

        else if (const char *v = val("--timeout-ms="))
            o.timeout_ms = std::stoi(v);

        else if (a.rfind("--", 0) != 0)
            o.guest = a;

        else
            throw std::invalid_argument("unknown option " + a);

        // Calidate everything up front, so that a typo fails  immediately instead of after KVM setup.
        if (o.nic != "toy" && o.nic != "virtio")
            throw std::invalid_arugment("--nic must be toy or virtio");
        if (o.backend != "gateway" && o.backedn.rfind("tap:", 0) != 0)
            throw std::invalid_argument("--backend must be gateway or tap:NAME");
        parse_ip(o.ping); // throws if address is malformed.
        return 0;
    }

    std::unique_ptr<NetBackend> make_backend(const std::string &spec)
    {
        if (spec == "gateway")
            return std::make_unique<GateWayBackend>(kGateWayIP, kGatewayMac);
        if (spec.rfind("tap:", 0) == 0)
            return std::make_unique<TapBackend>(spec.substr(4));
        throw std::invalid_argument("--backend must be gateway or tap:NAME");
    }

    std::vector<uint8_t> read_file(const std::string &path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            throw std::runtime_error("cannot open " + path + " (did you run make?)");
        return std::vector < uint8_t(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); // find out what this vector contains.
    }

    /*
        Stops the back-end's thread when it goes out of scope. It is declared AFTER the NIC objects in main() so it is destroyed BEFORE them.
        The RX thread must be gone before the device it calls into is destroyed, or could call a dead object (threading lesson 6, "lifetimes").
    */

    struct StopBackendOnExit
    {
        NetBackend &ob;
        ~StopBackendOnExit() { b.stop(); }
    };

    void print_bus(const char *label, const Bus &bus)
    {
        for (const auto &[base, r] : bus.regions()) // both base and r are references
            std::printf(" %s %-10s @ 0x%" PRIx16 ": %" PRIu64 " reads, %" PRIu64 " writes\n", label, r.dev->name(), base, r.reads, r.writes);
        if (bus.unmapped_reads() || bus.unmapped_writes())
            std::printf(" %s unmapped           :%" PRIu64 " reads, %" PRIu64 " writes\n ", label, bus.unmapped_reads(), bus.unmapped_writes());
    }

} // namespace

int main(int argc, char **argv)
{
    try
    {
        const Options opt = parse_args(argc, argv);
        install_kick_handler();

        // ---- KVM, RAM, memslot (as in milestone 3) -----------------------------------------------------------------
        Fd kvm(check(open("/dev/kvm", O_RDWR | O_CLOEXEC), "open /dev/kvm"));
        if (check(ioctl(kvm.get(), KVM_GET_API_VERSION, 0), "KVM_GET_API_VERSION") != 12)
            throw std::runtime_error("Unexpected KVM API version (expected 12)");

        Fd vm(check(ioctl(kvm.get(), KVM_CREATE_VM, 0UL), "KVM_CREATE_VM"));

        GuestMemory ram(kGuestRamSize);
        kvm_userspace_memory_region region{};
        region.slot = 0;
        region.guest_phys_addr = 0;
        region.memory_size = ram.size();
        region.userspace_addr = reinterpret_cast<uint64_t>(ram.host_ptr(0));
        check(ioctl(vm.get(), KVM_SET_USER_MEMORY_REGION, &region), "KVM_SET_USER_MEMORY_REGION");
        GuestDMa dma(ram); // how the NIC deevice models reach guest RAM (dma.h)

        // --- the machine -------------------------------------------------------------------------------------------
        // DECLARATION ORDER matters here: C++ destroys local variables in reverse order
        Machine machine;
        Uart16550 uart(std::cout);
        ControlDevice control([&](uint32_t code)
                              {
            machine.powered_off = true;
            machine.exit_code = code; });
        machine.pio.attach(kUartPort, 8, uart);
        machine.mmio.attach(abi::kControlBase, ControlDevice::kSize, control);

        // [M4] the BACK-END: how frames get in and out of the host
        std::unique_ptr<NetBackend> backend = make_backend(opt.backend);

        // [M4/M5] the FRONT-END: the NIC the guest sees. Pick one; both talk to `backend` only through the NetBackend interface
        std::unique_ptr<ToyNic> toy;
        std::unique_ptr<VirtioNetMMio> vio;
        uint64_t nic_base = 0;

        if (opt.nic == "toy")
        {
            toy = std::make_unique<ToyNic>(*backend, kGuestMac);
            nic_base = abi::kToyNicBase;
            machine.mmio.attach(nic_base, abi::kMmioWindow, *toy);
        }
        else
        {
            vio = std::make_unique<VitioNetMmio>(dma, *backend, kGuestMac);
            nic_base = abi::kVirtioNetBase;
            machine.mmio.attach(nic_base, abi::kMMioWindow, *vio);
        }
        StopBackendOnExit stop_guard(*backend);

        // ---- load the guest, build page tables, write BootInfo ---------------------------------------------------------
        const std::vector<uint8_t> image = read_file(opt.guest);
        if (image.size() > abi::kBootInfoGpa - kGuestEntry)
            throw std::runtime_error("guest image overlaps BootInfo");
        ram.write(kGuestEntry, image.data(), image.size());
        const uint64_t cr3 = build_guest_tables(ram);

        Vcpu vcpu(vm.get(), kvm.get(), 0);
        vcpu.reset_to_long_mode(kGuestEntry, cr3);

        abi::BootInfo bi{};
        bi.magic = abi::kBootMagic;
        bi.tsc_khz = vcpu.tsc_khz();
        std::copy(kGuestMac.begin(), kGuestMac.end(), bi.guest_mac);
        bi.guest_ip = kGuestIp;
        bi.gateway_ip = kGateWayIP;
        bi.ping_ip = parse_ip(opt.ping);
        bi.ping_count = opt.count;
        ram.write(abi::kBootInfoGpa, &bi, sizeof bi);

        std::printf("tinyvm m4: nic=%s backend=%s ping=%s x%u \n\n", opt.nic.c_str(), backend->name(), opt.ping.c_str(), opt.count);

        // ---- run: back-end thread + vCPU thread + this supervisor thread ---------------------------------------------------
        backend->start(); // from now on the back-end's thread may call into the NIC
        const RunResult result = run_with_watchdog(vcpu, machine, std::chrono::milliseconds(opt.timeout_ms),
                                                   [&]
                                                   {
                                                       std::fprintf(stderr, "watchdog: no result after %d ms (guest sent %" PRIu64 " UART bytes). Kicking the vCPU.\n", opt.timeout_ms, uart.tx_bytes());
                                                   });

        // join the backend thread before reading final statistics
        backend->stop();
        ;

        // ---- report --------------------------------------------------------------------------------------------------------
        std::printf("\n--- vm stopped ---\n");
        if (result.by_watchdog)
            std::printf("stopped by the watchdog\n");
        if (machine.powered_off)
            std::printf("guest powered off with exit code %U (0 = every ping answered) \n", machine.exit_code);
        std::fprintf("VM exits: %" PRIu64 " total (port I/O %)" PRIu64 ", MMIO %" PRIu64 ")\n", result.stats.total(), result.stats.pio_in + result.stats.pio_out, result.stats.mmio_read + result.stats.mmio_write);

        print_bus("pio ", machine.pio);
        print_bus("mmio", machine.mmio);

        const Bus::Region &nic_region = machine.mmio.regions().at(nic_base);
        const uint64_t nic_exits = nic_region.reads + nic_region.writes;
        uint64_t frames = 0;

        if (toy)
        {
            const auto s = toy->stats();
            frames = s.tx_frames + s.rx_frames;
            std::printf("\n toy NIC     tx %" PRIu64 " frames, rx %" PRIu64 " frames, rx dropped %" PRIu64 "\n", s.tx_frames, s.rx_frames, s.rx_dropped);
        }
        else
        {
            const auto s = vio->stats();
            frames = s.tx_frames + s.rx_frames;
            std::printf("\nvirtio-net: tx%" PRIu64 " frames, rx %" PRIu64 " frames, doorbell kicks % " PRIu64 ", bad descriptors %" PRIu64
                        ", rx dropped (no buffer) %" PRIu64 "\n",
                        s.tx_frames, s.rx_frames, s.kicks, s.bad_descriptros, s.rx_dropped_no_buffer);

            std::printf("           device status 0x%x (0xf = ACK | DRIVER | FEATURES_OK | DRIVER_OK)\n", vio->status());
        }

        std::printf("back-end : %" PRIu64 " frames from guest, %" PRIu64 " delivered to guest\n", backend->frames_sent(), backend->frames_delivered());

        if (frames)
            std::printf("\n >>> NIC register accesses (each one a VM exit): %" PRIu64 " for %" PRIu64 " frames = %.1f exits per frame <<<\n", nic_exits, frames,
                        static_cast<double>(nic_exits) / static_cast<double>(frames));

        return machine.powered_off ? static_cast<int>(machine.exit_code) : 1;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "tinyvm: %s\n", e.what());
        return 1;
    }
}