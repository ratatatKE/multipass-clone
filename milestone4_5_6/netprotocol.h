/*
    netprotocol.h - just enough Ethernet, ARP, IPv4 and ICMP to ping something.

    Used by the guest (to build ARP requests and ICMP echo requests)
    and by the host's GatewayBackend (to answer them).

    This file is freestanding with no libc and also compiles within the guest.

    Byte order: 
    -----------
    x86 is little-endian, network protocols are big-endian ("network byte order"). 
    Multi-byte header fields therefore need a swap when we read or write them.

*/

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace net {

    constexpr uint16_t kEtherArp        = 0x0806;
    constexpr uint16_t kEtherIpv4       = 0x0800;
    constexpr uint8_t  kProtoIcmp       = 1;
    constexpr uint8_t  kIcmpEchoReply   = 0;
    constexpr uint8_t  kIcmpEchoRequest = 8;
    constexpr uint16_t kArpRequest      = 1;
    constexpr uint16_t kArpReply        = 2;
    constexpr size_t   kMaxFrame        = 1536;


    inline uint16_t bswap16(uint16_t v) { return static_cast<uint16_t>((v << 8) | (v >> 8)); }

    // These structs mirror the  the bytes on the wire exactly.
    
    struct __attribute__((packed)) EthHdr{
        uint8_t dst[6];
        uint8_t src[6];
        uint16_t etherType;                                             // big-endian on the wire
    };

    struct __attribute__((packed)) ArpPkt {
        uint16_t htype;     // 1 = Ethernet
        uint16_t ptype;     // 0x0800 = IPv4
        uint8_t hlen;       // 6 i.e. header length
        uint8_t plen;       // 4
        uint16_t oper;      // 1 = who-has (request), 2 = is-at (reply)
        uint8_t sha[6];     // sender hardware (MAC) address
        uint8_t spa[4];     // sender protocol (IP) address
        uint8_t tha[6];     // target hardware address
        uint8_t tpa[4];     // target protocol address

    };

    struct __attribute__((packed)) Ipv4Hdr {
        uint8_t ver_ihl;    // 0x45 = version 4, header length 5 words (20 bytes)
        uint8_t tos;
        uint16_t total_len;
        uint16_t id;
        uint16_t frag;
        uint8_t ttl;
        uint8_t proto;
        uint16_t csum;
        uint8_t src[4];
        uint8_t dst[4];
    };

    struct __attribute__ ((packed)) IcmpEcho {
        uint8_t type;
        uint8_t code;
        uint8_t csum;
        uint16_t id;
        uint16_t seq;
    };


    


} // namespace net