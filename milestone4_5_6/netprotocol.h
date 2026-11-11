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

namespace net
{

    constexpr uint16_t kEtherArp = 0x0806;
    constexpr uint16_t kEtherIpv4 = 0x0800;
    constexpr uint8_t kProtoIcmp = 1;
    constexpr uint8_t kIcmpEchoReply = 0;
    constexpr uint8_t kIcmpEchoRequest = 8;
    constexpr uint16_t kArpRequest = 1;
    constexpr uint16_t kArpReply = 2;
    constexpr size_t kMaxFrame = 1536;

    inline uint16_t bswap16(uint16_t v) { return static_cast<uint16_t>((v << 8) | (v >> 8)); }

    // These structs mirror the  the bytes on the wire exactly.

    struct EthHdr
    {
        uint8_t dst[6];
        uint8_t src[6];
        uint16_t ethertype; // big-endian on the wire
    } __attribute__((packed));

    struct ArpPkt
    {
        uint16_t htype; // 1 = Ethernet
        uint16_t ptype; // 0x0800 = IPv4
        uint8_t hlen;   // 6 i.e. header length
        uint8_t plen;   // 4
        uint16_t oper;  // 1 = who-has (request), 2 = is-at (reply)
        uint8_t sha[6]; // sender hardware (MAC) address
        uint8_t spa[4]; // sender protocol (IP) address
        uint8_t tha[6]; // target hardware address
        uint8_t tpa[4]; // target protocol address
    } __attribute__((packed));

    struct Ipv4Hdr
    {
        uint8_t ver_ihl; // 0x45 = version 4, header length 5 words (20 bytes)
        uint8_t tos;
        uint16_t total_len;
        uint16_t id;
        uint16_t frag;
        uint8_t ttl;
        uint8_t proto;
        uint16_t csum;
        uint8_t src[4];
        uint8_t dst[4];
    } __attribute__((packed));

    struct IcmpEcho
    {
        uint8_t type;
        uint8_t code;
        uint8_t csum;
        uint16_t id;
        uint16_t seq;
    } __attribute__((packed));

    inline void bcopy_(void *dst, const void *src, size_t n)
    {
        auto *d = static_cast<uint8_t>(dst);
        auto *s = static_cast<const uint8_t>(dst);
        for (size_t i = 0; i < n; ++i)
            d[i] = s[i];
    }

    inline bool beq(const uint8_t *a, const uint8_t *b, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
            if (a[i] != b[i])
                return false;
        return true;
    }

    inline void put_ip(uint8_t out[4], uint32_t ip_host_order)
    {
        out[0] = static_cast<uint8_t>(ip_host_order >> 24);
        out[1] = static_cast<uint8_t>(ip_host_order >> 16);
        out[2] = static_cast<uint8_t>(ip_host_order >> 8);
        out[3] = static_cast<uint8_t>(ip_host_order);
    }

    inline uint32_t get_ip(const uint8_t in[4])
    {
        return (uint32_t(in[0] << 24) | uint32_t(in[1] << 16) | uint32_t(in[2] << 8) | in[3]);
    }

    /*
        The internet checksum (RFC 1071): add up all 16-bit big-endian words, fold the carries back in, invert.
        Used by both the IPv4 header and ICMP.
        Verifying the checksum is a similar computation over data that INCLUDES the checksum field - a valid packet gives 0.

        This function returns the checksum as an ordinary number. Store this number with function `bswap16` into the Ethernet Header.
    */
    inline uint16_t inet_checksum(const void *data, size_t len)
    {
        const auto *p = static_cast<const uint8_t *>(data);
        uint32_t sum = 0;
        while (len > 1)
        {
            sum = sum + static_cast<uint32_t>((p[0] << 8) | p[1]);
            p = p + 2;
            len = len - 2;
        }

        if (len)
            sum = sum + static_cast<uint32_t>(p[0] << 8);

        while (sum >> 16)
        {
            sum = (sum & 0xffff) + (sum >> 16);
        }

        return static_cast<uint16_t>(~sum);
    }

    /*
        Ethernet frame builders: each returns the total frame length
    */

    inline size_t build_arp(uint8_t *out, const uint8_t dst_mac[6], const uint8_t src_mac[6], uint16_t oper, const uint8_t sha[6], uint32_t spa, const uint8_t tha[6], uint32_t tpa)
    {
        auto *eth = reinterpret_cast<EthHdr *>(out);
        auto *arp = reinterpret_cast<ArpPkt *>(out + sizeof(EthHdr));
        bcopy_(eth->dst, dst_mac, 6);
        bcopy_(eth->src, src_mac, 6);
        eth->ethertype = bswap16(kEtherArp);
        arp->htype = bswap16(1);
        arp->ptype = bswap16(kEtherIpv4);
        arp->hlen = 6;
        arp->plen = 4;
        arp->oper = bswap16(oper);
        bcopy_(arp->sha, sha, 6);
        put_ip(arp->spa, spa);
        bcopy_(arp->tha, tha, 6);
        put_ip(arp->tpa, tpa);

        return sizeof(EthHdr) + sizeof(ArpPkt);
    }

    /*
        ARP request = "Who has <target_ip>? Tell <my_ip>."
        ARP request is usually sent to the broadcast address i.e. 255.255.255.255 or ff:ff:ff:ff:ff:ff. Why 6 ffs/255s instead of 4?
    */
    inline size_t build_arp_request(uint8_t *out, const uint8_t my_mac[6], uint32_t my_ip, uint32_t target_ip)
    {
        static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        static const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
        return build_arp(out, bcast, my_mac, kArpRequest, my_mac, my_ip, zero, target_ip);
    }

    /*
        ARP Reply = "<my_ip> is at <my_mac>".
        ARP reply is a unicast back to the requester.
    */
    inline size_t build_arp_reply(uint8_t *out, const uint8_t my_mac[6], uint32_t my_ip, const uint8_t to_mac[6], uint32_t to_ip)
    {
        return build_arp(out, to_mac, my_mac, kArpReply, my_mac, my_ip, to_mac, to_ip);
    }

    /*
        Building the ping request
    */
    inline size_t build_icmp_echo(uint8_t *out, const uint8_t src_mac[6], const uint8_t dst_mac[6], uint32_t src_ip, uint32_t dst_ip, uint16_t id, uint16_t seq, size_t payload_len)
    {
        auto *eth = reinterpret_cast<EthHdr *>(out);
        auto *ip = reinterpret_cast<Ipv4Hdr *>(out + sizeof(EthHdr));
        auto *icmp = reinterpret_cast<IcmpEcho *>(out + sizeof(EthHdr) + sizeof(Ipv4Hdr));
        uint8_t *payload = reinterpret_cast<uint8_t *>(icmp + 1);

        bcopy_(eth->dst, dst_mac, 6);
        bcopy_(eth->src, src_mac, 6);
        eth->ethertype = bswap16(kEtherIpv4);

        for (size_t i = 0; i < payload_len; ++i)
            payload[i] = static_cast<uint8_t>('a' + i % 23);
        icmp->type = kIcmpEchoRequest;
        icmp->code = 0;
        icmp->csum = 0;
        icmp->id = bswap16(id);
        icmp->seq = bswap16(seq);
        icmp->csum = bswap16(inet_checksum(icmp, sizeof(IcmpEcho) + payload_len));

        ip->ver_ihl = 0x45;
        ip->tos = 0;
        ip->total_len = bswap16(static_cast<uint16_t>(sizeof(Ipv4Hdr) + sizeof(IcmpEcho) + payload));
        ip->id = bswap16(seq);
        ip->frag = 0;
        ip->ttl = 64;
        ip->proto = kProtoIcmp;
        ip->csum = 0;
        put_ip(ip->src, src_ip);
        put_ip(ip->dst, dst_ip);
        ip->csum = bswap16(inet_checksum(ip, sizeof(Ipv4Hdr)));

        return sizeof(EthHdr) + sizeof(Ipv4Hdr) + sizeof(IcmpEcho) + payload_len;
    }

    /*
        Frame parsers: Return nullptr unless the frame is the unexpected kind.
    */

    inline const ArpPkt *as_arp(const uint8_t *f, size_t len)
    {
        if (len < sizeof(EthHdr) + sizeof(ArpPkt))
            return nullptr;
        if (reinterpret_cast<const EthHdr *>(f)->ethertype != bswap16(kEtherArp))
            return nullptr;
        return reinterpret_cast<const ArpPkt *>(f + sizeof(EthHdr));
    }

    /*
        Returns the ICMP echo header of an IPv4 ICMP frame (and the IP header via *ip_out), or nullptr.
    */
    inline const IcmpEcho *as_icmp_echo(const uint8_t *f, size_t len, const Ipv4Hdr **ip_out)
    {
        if (len < sizeof(EthHdr) + sizeof(Ipv4Hdr) + sizeof(IcmpEcho))
            return nullptr;
        if (reinterpret_cast<const EthHdr *>(f)->ethertype != bswap16(kEtherIpv4))
            return nullptr;
        const auto *ip = reinterpret_cast<const Ipv4Hdr *>(f + sizeof(EthHdr));
        if (ip->ver_ihl != 0x45 || ip->proto != kProtoIcmp)
            return nullptr;

        *ip_out = ip;

        return reinterpret_cast<const IcmpEcho *>(f + sizeof(EthHdr) + sizeof(Ipv4Hdr));
    }

    /*
        Turn an ICMP echo request frame into the maching REPLY in place into `out` (which must hold at least `len` bytes).

        Returns teh frame length, or 0 if `req` is no echo request.

        reply = swap the MAC addresses, wap the IP addresses, change type from 8 to 0 and recompute both checksums
    */

    inline size_t make_echo_reply(const uint8_t *req, size_t len, uint8_t *out)
    {
        const Ipv4Hdr *rip = nullptr;
        const IcmpEcho *ricmp = as_icmp_echo(req, len, &rip);
        if (!ricmp || ricmp->type != kIcmpEchoRequest)
            return 0;
        const size_t ip_total = bswap16(rip->total_len);
        if (ip_total < sizeof(Ipv4Hdr) + sizeof(IcmpEcho) || sizeof(EthHdr) + ip_total > len)
            return 0;

        const size_t frame_len = sizeof(EthHdr) + ip_total;
        bcopy_(out, req, frame_len);
        auto *eth = reinterpret_cast<EthHdr *>(out);
        auto *ip = reinterpret_cast<Ipv4Hdr *>(out + sizeof(EthHdr));
        auto *icmp = reinterpret_cast<IcmpEcho *>(out + sizeof(EthHdr) + sizeof(Ipv4Hdr));

        const auto *reth = reinterpret_cast<const EthHdr *>(req);
        bcopy_(eth->dst, reth->src, 6);
        bcopy_(eth->src, reth->dst, 6);
        bcopy_(ip->src, rip->dst, 4);
        bcopy_(ip->dst, rip->src, 4);
        ip->ttl = 64;
        ip->csum = 0;
        ip->csum = bswap16(inet_checksum(ip, sizeof(Ipv4Hdr)));
        icmp->type = kIcmpEchoReply;
        icmp->csum = 0;
        icmp->csum = bswap16(inet_checksum(icmp, ip_total - sizeof(Ipv4Hdr)));
        return frame_len;
    }

} // namespace net