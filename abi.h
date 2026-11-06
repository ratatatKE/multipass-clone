/*
    abi.h - the contract between the hypervisor's device models and the guest's drivers.

    In real life, the contract is a DATASHEET or a SPECIFICATION ( for VirtIO, this spec is the OASIS virtio spec, section 4.2 "Virtio over MMIO" )
    and driver & device authors each read it independently and implement their own half. In this project, both halves LIVE IN ONE REPOSITORY.
    So we write the contract down ONCE in this file and include it from both sides.That is a shortcut for the toy NIC (we invented it) but also a 
    nice way to see that a device model and a driver are two impementations of the same table.

    This header is compiled into BOTH the host program (g++ with libc) and the bare-metal guest (freestanding) and therefore it can only use <stdint.h>

*/

#pragma once
#include <stdint.h>

namespace abi {

/*
    MACHINE MEMORY MAP (guest-physical addresses)
*/
constexpr uint64_t kControlBase     = 0xd0000000;           // ControlDevice (poweroff), from milestone 2
constexpr uint64_t kToyNicBase      = 0xd0001000;           // [M4] toy NIC front-end
constexpr uint64_t kVirtioNetBase   = 0xd0002000;           // [M5] virtio-net front-end
constexpr uint64_t kMmioWindow      = 0x1000;               // each device owns one 4 KiB window
constexpr uint64_t kCtlPoweroff     = 0x08;                 // ControlDevice register: write exit code

/*
    All three MMIO windows sit inside the single uncached 2 MiB page that paging.h maps at 0xd0000000 and so the guest can reach them with ordinary stores and loads
    and so can the host.
*/

constexpr uint64_t kBootInfoGpa     = 0xf000;               // VMM -> guest parameters (like Linux's boot_params)
constexpr uint64_t kNetDmaGpa       = 0x40000;              // 64 KiB of RAM the guest drivers use for rings and buffers.
constexpr uint64_t kNetDmaSize      = 0x10000;

/*
    BOOT INFO: What the VMM tells the guest before the guest starts. The guest has no commandline, no firmware and no DHCP, so the VMM simply writes this construct into
    the guest's RAM.
    All IPv4 addresses here are in HOST byte order (10.0.2.15 == 0x0a00020f).

*/

constexpr uint32_t kBootMagic = 0x544f4f42;                 // bytes 'B','O','O','T' on little-endian CPU

struct BootInfo {
    uint32_t magic;
    uint32_t tsc_khz;                                       // timestamp-counter frequency so that the guest can measure time
    uint8_t guest_mac[6];
    uint16_t reserved;
    uint32_t guest_ip;
    uint32_t gateway_ip;
    uint32_t ping_ip;
    uint32_t ping_count;
};

/*
    [M4] Toy NIC: a deliberately old-fashioned network card i.e "Programmed I/O style".
    The CPU copies every frame to and from the card 4 bytes at a time through a data register.
    Every one of those register acccesses is a VM exit. The cost is the whole point of M4 because M5 virtio exists to avoid these exits.
*/

namespace toy {
    constexpr uint32_t kMagic       = 0x43494e54;                   // "TNIC"

    constexpr uint32_t kRegMagic    = 0x00;                         // RO   must read kMagic
    constexpr uint32_t kRegStatus   = 0x04;                         // RO   bit0 = link up, bit1 = a received frame is waiting
    constexpr uint32_t kRegMacLo    = 0x08;                         // RO   MAC bytes 0..3, little-endian
    constexpr uint32_t kRegMacHi    = 0x0c;                         // RO   MAC bytes 4..5
    constexpr uint32_t kRegTxLen    = 0x20;                         // WO   start a new outgoing frame of this many bytes
    constexpr uint32_t kRegTxData   = 0x14;                         // WO   append 4 bytes of the frame (little-endian dword)    
    constexpr uint32_t kRegTxGo     = 0x18;                         // WO   write 1: transmit the frame that was assembled.
    constexpr uint32_t kRegRxLen    = 0x20;                         // RO   length of the frame at the head of the RX queue, else 0
    constexpr uint32_t kRegRxData   = 0x24;                         // RO   next 4 bytes of the head frame (advances on each read!)    
    constexpr uint32_t kRegRxPop    = 0x28;                         // WO   write 1: discard the head frame

    constexpr uint32_t kStatusLinkUp    = 1u << 0;
    constexpr uint32_t kStatusRxReady   = 1u << 1;
}   // namespace toy


/*
    [M5] VIRTIO over MMIO, "modern" version 2 (Virtio 1.x sepc, section 4.2.2).
    Register offsets and bit values below come from the specification, so a real linux virtio_mmio driver would find them where it expects them.
    See Linux source drivers/virtio/virtio_mmic.c
*/

namespace virtio {
    constexpr uint32_t kMagic       = 0x74726976;                   // virt
    constexpr uint32_t kVersion     = 2;
    constexpr uint32_t kDeviceNet   = 1;                            // device id 1 == network card
    constexpr uint32_t kVendor      = 0x4d565454;                   // arbitrary vendor id

    constexpr uint32_t kRegMagicValue           = 0x000;                // RO
    constexpr uint32_t kRegversion              = 0x004;                // RO
    constexpr uint32_t kRegDeviceId             = 0x008;                // RO
    constexpr uint32_t kRegVendorId             = 0x00c;                // RO
    constexpr uint32_t kRegDeviceFeatures       = 0x010;                // RO   32 bits of features, chosen by DeviceFeaturesSel
    constexpr uint32_t kRegDeviceFeaturesSel    = 0x014;                // WO
    constexpr uint32_t kRegDriverFeatures       = 0x020;                // WO   features the driver accepts
    constexpr uint32_t kRegDriverFeaturesSel    = 0x024;                // WO
    constexpr uint32_t kRegQueueSel             = 0x030;                // WO   which queue the following queue registers talk about
    constexpr uint32_t kRegQueueNumMax          = 0x034;                // RO   largest queue size the device supports (0= no such queue)    
    constexpr uint32_t kRegQueueNum             = 0x038;                // WO   queue size the driver chose
    constexpr uint32_t kRegQueueReady           = 0x044;                // RW   1 = queue is set up and usable
    constexpr uint32_t kRegQueueNotify          = 0x050;                // WO   the DOORBELL: write a queue index
    constexpr uint32_t kRegInterruptStatus      = 0x060;                // RO
    constexpr uint32_t kRegInterruptAck         = 0x064;                // WO
    constexpr uint32_t kRegStatus               = 0x070;                // RW   the handshake state machine
    constexpr uint32_t kRegQueueDescLow         = 0x080;                // WO   guest-physical address of the descriptor table
    constexpr uint32_t kRegQueueDescHigh        = 0x084;
    constexpr uint32_t kRegQueueDriverLow       = 0x090;                // WO   ... of the "available" ring (driver -> device )
    constexpr uint32_t kRegQueueDriverHigh      = 0x094;
    constexpr uint32_t kRegQueueDeviceLow       = 0x0a0;                // WO   ... of the "used" ring (device -> driver )
    constexpr uint32_t kRegQueueDeviceHigh      = 0x0a4;
    constexpr uint32_t kRegConfigGeneration     = 0x0fc;                // RO
    constexpr uint32_t kRegConfig               = 0x100;                // RO   device-specific config; for net; 6-byte MAC first


    // Device status bits (the driver ORs them in, in this order, during the handshake).
    constexpr uint32_t kStatusAcknowledge       = 1;                    // "I see a device"
    constexpr uint32_t kStatusDriver            = 2;                    // I "know how to drive it"         
    constexpr uint32_t kStatusDriverOk          = 4;                    // "I am ready; start"
    constexpr uint32_t kStatusFeaturesOk        = 8;                    // "I accept your features"
    constexpr uint32_t kStatusNeedsReset        = 0x40;
    constexpr uint32_t kStatusFailed            = 0x80;

    // Feature bits (64-bit feature word, read 32 bits at a time).
    constexpr uint64_t kFeatureNetMac           = 1ull << 5;            // device has a fixed MAC in config space
    constexpr uint64_t kFeatureVersion1         = 1ull << 32;           // "modern" virtio; mandatory for us

    // One entry of the descriptor table: a buffer of `len` bytes at guest-physical `addr`
    constexpr uint16_t kDescNext                = 1;                    // this descriptor continues in `next` (a chain)
    constexpr uint16_t kDescWrite               = 2;                    // the DEVICE writes into this buffer (otherwise it only reads)

    struct Desc {
        uint64_t addr;
        uint32_t len;
        uint16_t flags;
        uint16_t next;
    };

    static_assert(sizeof(Desc) == 16, "descriptor layout is fixed by the spec");

    /*
        Every packet in either direction is preceeded by this header (12 bytes with VERSION_1)/
        We use no offloads, so the guest sends it zeroed and the device returns it zeroed.
        num_buffers = 1
    */
   struct NetHdr {
        uint8_t flags;
        uint8_t gso_type;
        uint16_t hdr_len;
        uint16_t gso_size;
        uint16_t csum_start;
        uint16_t csum_offset;
        uint16_t num_buffers;
   };

   static_assert(sizeof(NetHdr) == 12, "net header layout is fixed by the spec");

   constexpr uint16_t kQueueMax = 16;                           // largest queue the device accepts.
   constexpr uint16_t kQueueSize = 8;                           // what our guest driver actually asks for (power of two)
   constexpr uint32_t kRxQueue = 0;                             // receiveq: device -> guest frames
   constexpr uint32_t kTxQueue = 1;                             // transmitq: guest -> device frames

} // namespace virtio

} // namespace abi