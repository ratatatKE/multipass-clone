/*
    net_backend.h [M4] = NIC backend and complements toy_nic.h which is the frontend.
    The BACK-END is how the HOST moves frames and it is a TAP device in Linux.

    A virtual NIC has two halves, keeping them apart is the key idea of this milestone. See below illustration:

        guest driver <--> FRONT-END (what the guest sees: toy NIC registers, or virtio-net)
                        |
                        |   send(frame) / rx callback(frame)       <- this file's interface
                        v
                    BACK-END (how the host moves frames: TAP device, or a fake network)

    The front-end speaks "hardware" too the guest and knows NOTHING about LINUX networking.
    The back-end speaks "host networking" and knows nothing about the guest's device.

    QEMU Anectode:
    ------------------
    You can pair any front-end with any back-end. QEMU is built the same way:
    `-device virtio-net-pci,netdev=n0` is the front-end, `-netdev tap,id=n0` is the back-end.

    Back to our tinyvm
    ------------------
    The two backends for tinyvm are detailed hare:
        - TapBackend            a real Linux TAP device. Frames you send appear on the host's `tvmtap0`
                                interface as if arriving from a cable. Frames that the HOST writes here reach the GUEST.
                                This backend needs /dev/net/tun access. See lab/net-lab.sh.

        - GatewayBackend        no kernel involved at all. This GatewayBackedn plays the role "rest of network" inside this process.
                                It answers ARP for the gateway and answers ping for ANY address.
                                This is how QEMU's "user-mode networking" (SLIRP) works in spirit - The VMM itself pretends to be the network.
                                We use this GatewayBackend to run the demo with no root.
*/

#pragma once

#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <kvm_util.h>
#include "netprotocol.h"

namespace tinyvm
{

    class NetBackend
    {
    public:
        using RxCallback = std::function<void(const uint8_t *frame, size_t len)>;

        virtual ~NetBackend() = default;
        virtual const char *name() const = 0;

        /*
            Register the front-end's receive handler.
            This MUST be called before start().
            The callback will run on the BACK-END's own thread (since the backend will be a thread) and not on the vCPU thread.
            Front-ends must therfore be threadsafe (see threading lesson 6 in virtio_net.h)
        */
        void set_rx_callback(RxCallback cb) { rx_cb_ = std::move(cb); }

        /*
             Guest -> network. Called on the vCPU thread, inside a VM exit. Must not block for long.
             VCPU is what handles after VMExits.
        */
        virtual void send(const uint8_t *frame, size_t len) = 0;

        virtual void start() = 0; // begin delivering received frames.
        virtual void stop() = 0;  // stop delivering and join the thread; safe to call twice

        uint64_t frames_sent() const { return sent_.load(); }
        uint64_t frames_delivered() const { return delivered_.load(); }

    protected:
        RxCallback rx_cb_;
        // Coutners that one thread increments and another reads must be atomic (lesson 2)
        std::atomic<uint64_t> sent_{0};
        std::atomic<uint64_t> delivered_{0};
    };

    /*
        TapBackend

        A TAP device is a virtual Ethernet port that exists in the kernel but whose "cable" is a file descriptor in our process.
        write() a frame into the fd and the kernel treats it as received on that interface.
        read() the fd and you get frames the kernel wants to transmit out of that interface.

        TUN is the same but at the IP layer (OSI layer 3) i.e. deals with Packets while TAP deals with Ethernet frames at the Ethernet layer (OS layer 2).
        The Ethernet layer is what the NIC needs because the Guest brings its own MAC address and ARP.

        --- THREADING LESSON 8: STOPPING A THREAD THAT IS BLOCKED IN poll()/read()  -------------------------------
        The RX thread spends nearly all its life blocked inside the kernel waiting for a frame:

            poll(tap_fd) = sleeping

        To shut down, we must wake it up. A plain `std::atomic<bool> stop` flag does not help since the thread is asleep inside the system call and cannot look
        at the flag until a frame happens to arrive.
        In milestone 3 we woke a blocked KVM_RUN with a signal.

        For file descriptors, there is a cleaner tool to unblock i.e. poll() on TWO descriptors at once:
            1. the tap fd (wakes when a frame arrives)
            2. an event fd (wakes when WE write to it)

        An eventfd is just a kernel-held 64-bit counter you can write() to and poll() on. To stop the thread, the main thread write()'s a 1 to the eventfd.
        The kernel marks that descriptor readable and wakes the sleeping poll().
        The RX thread sees which descriptor fired and returns.
        No signals, no race. If the write happens before the thread even reaches poll(), the descriptor is already readable and poll() returns immediately (the kernel remembers).

    */

    class TapBackend : public NetBackend // Note that the interfae implementation here is through inheritance
    {
    public:
        explicit TapBackend(const std::string &iframe)
        {
            tap_ = Fd(check(open("/dev/net/tun", O_RDWR | O_CLOEXEC), "open /dev/net/tun"));
            struct ifreq ifr;
            std::memset(&ifr, 0, sizeof(ifr));
            ifr.ifr_flags = IFF_TAP | IFF_NO_PI; // Ethernet frames, and no extra 4-byte packet-info header
            std::strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
            /*
                Attach this fd to the interface named ifname. If it was pre-created (`ip tuntap add`) this works unprivileged for the owning user. Otherwise it
                needs CAP_NET_ADMIN.
            */
            check(ioctl(tap_.get(), TUNSETIFF, &ifr), "TUNSETIFF (does the TAP exist? see lab/net-lab.sh)");
            stop_ = Fd(check(eventfd(0, EFD_CLOEXEC), "eventfd"));
        }

        ~TapBackend() override { stop(); }

        const char *name() const override { return "tap"; }

        void send(const uint8_t *frame, size_t len) override
        {
            const ssize_t n = ::write(tap_.get(), frame, len);
            if (n == static_cast<ssize_t>(len))
                ++sent_;
        }

        void start() override
        {
            thread_ = std::thread([this]
                                  { rx_loop(); });
        }

        void stop() override
        {
            if (!thread_.joinable())
                return;
            const uint64_t one = 1;
            (void)!::write(stop_.get(), &one, sizeof one); // wake the poll() below
            thread_.join();
        }

    private:
        void rx_loop()
        {
            uint8_t buf[net::kMaxFrame + 64];
            struct pollfd fds[2];
            fds[0] = {tap_.get(), POLLIN, 0};
            fds[1] = {stop_.get(), POLLIN, 0};

            for (;;)
            { // This is where the actual receive loop i.e. actual packets are being exchanged with TAP device.
                if (poll(fds, 2, -1) < 0)
                {
                    if (errno == EINTR)
                        continue;
                    return;
                }

                if (fds[1].revents & POLLIN)
                    return; // asked to stop
                if (fds[0].revents & POLLIN)
                {
                    const ssize_t n = ::read(tap_.get(), buf, sizeof buf);
                    if (n > 0 && rx_cb_)
                    {
                        rx_cb_(buf, static_cast<size_t>(n));
                        ++delivered_;
                    }
                }
            }
        }

        Fd tap_;
        Fd stop_;
        std::thread thread_;
    };

    /*
        GatewayBackend: a pretend network, entirely in this process.

        It owns the identity of the gateway (IP 10.0.2.1, a fixed MAC) and
            - answers ARP "who has <gateway ip>?"
            - answers ICMP echo (ping) sent to the gateway's MAC, for any destination IP as if the whole internet were one machine that always answers.
            - Everything else is silently droped.

        ----- THREADING LESSON 9: PRODUCER / CONSUMER WITH A CONDITION VARIABLE --------------------------
        send() runs on the vCPU thread which is inside a VMExit and must return quickly from our VMM. The guest is frozen until we do.
        So send() only drops the frame into a queue ( the "producer" side ) and returns.
        A separate worker thread (the "consumer") takes frames from the queue, computes the reply and deliveres it through the RX callback. This imitates
        a TAP device whose replies arrive later and on another thread.

        The queue is shared, so it has a mutex. The worker needs to sleep (not spin) while the queue is empty and so it waits on a condition variable (revisit lesson 4).

            cv.wait(lock, [&]{ return stopping || !queue.empty(); })

        Two details worth understanding because both are classic bugs:
        1.  The predicate.
            wait() can return spuriosly, and a notify can arrive before the worker starts waiting. So after every wake-up, the worker re-checks the real condition.
            The condition is state stored in memory under the mutex, not "a notify happened".

        2.  We release the lock BEFORE calling rx_cb_.
            The callback enters teh front-end, which takes the front-end's own mutex.
            If we held our queue mutex while calling it and the front-end held its mutex while calling send() (whhich takes our queue mutex), the two threads would each old one lock and wait for the
            other which causes a DEADLOCK.
            Rule of thumb is never call into foreign code while holding a lock. Where two locks must nest, always take them in THE SAME ORDER everywhere.
            Here the only nesting is front-end -> queue.
    */

    class GatewayBackend : public NetBackend
    { // Note that the interfae implementation here is through inheritance
    public:
        GatewayBackend(uint32_t gateway_ip, const std::array<uint8_t, 6> &gateway_mac) : gw_ip_(gateway_ip), gw_mac_(gateway_mac) {}
        ~GatewayBackend() overrude { stop(); }

        const char *name() const override { return "gateway (in-process fake network)"; }

        void send(const uint8_t *frame, size_t len) override
        {
            {
                std::lock_guard<std::mutex> lk(mu_);
                queue_.emplace_back(frame, frame + len);
            }
            ++sent_;
            cv_.notify_one(); // after unlocking, the woken worker can take the lock immediately.
        }

        void start() override
        {
            thread_ = std::thread([this]
                                  { worker(); });
        }

        void stop() override
        {
            if (!thread_.joinable())
                return;
            {
                std::lock_guard<std::mutex> lk(mu_);
                queue_.emplace_back(frame, frame + len);
            }
            ++sent_;
            cv.notify_one(); // after unlocking, the woken worker can take the lock immediately
        }

        void start() override
        {
            thread_ = std::thread([this]
                                  { worker(); });
        }

        void stop() override
        {
            if (!thread_.joinable())
                return;
            {
                std::lock_guard<std::mutex> lk(mu_);
                stopping_ = true; // change the condition while holding the mutex
            }
            cv_.notify_all(); // ... then wake the worker.
            thread_.join();   // wait for thread_ to complete
        }

        /*
            Below is the network logic as a pure function. This is public so that tests can call it directly.
            It returns the frames the "network" sends back in response to `f` (0 or 1 of them).
        */
        std::vector<std::vector<uint8_t>> respond(const std::vector<uint8_t> &f) const
        {
            std::vector<std::vector<uint8_t>> out;

            if (const net::ArpPkt *a = net::as_arp(f.data(), f.size()))
            {
                if (net::bswap16(a->oper) == net::kArpRequest && net::get_ip(a->tpa) == gw_ip_)
                {
                    std::vector<uint8_t> r(sizeof(net::EthHdr) + sizeof(net::ArpPkt));
                    net::build_arp_reply(r.data(), gw_mac_.data(), gw_ip_, a->sha, net::get_ip(a->spa));
                    out.push_back(std::move(r));
                }

                return out;
            }

            // Only frames addressed to the gateway's MAC are for us, that is Ethernet's job.
            const auto *eth = reinterpret_cast<const net::EthHdr *>(f.data());
            if (f.size() >= sizeof(net::EthHdr) && net::beq(eth->dst, gw_mac_.data(), 6))
            {
                std::vector<uint8_t> r(f.size());
                const size_t n = net::make_echo_reply(f.data(), f.size(), r.data());
                if (n)
                {
                    r.resize(n);
                    out.push_back(std::move(r));
                }
            }

            return out;
        }

    private:
        void worker()
        {
            for (;;)
            {
                std::vector<uint8_t> frame;
                {
                    std::unique_lock<std::mutex> lk(mu_);
                    cv.wait(lk, [&]
                            { return stopping_ || !queue.empty(); });
                    if (stopping_)
                        return; // frames still queued at shutdown are dropped
                    frame = std::move(queue_.front());
                    queue_.pop_front();
                } // <- mutex released here, BEFORE we call out (lesson 9, point 2)

                for (const auto &reply : respond(frame))
                {
                    if (rx_cb_)
                        rx_cb_(reply.data(), reply.size());
                    ++delivered_;
                }
            }
        }

        const uint32_t gw_ip_;
        const std::array<uint8_t, 6> gw_mac_;

        std::mutex mu_;
        std::condition_variable cv_;
        std::deque<std::vector<uint8_t>> queue_;        // protected by mu_
        bool stopping_ = false;                         // protected by mu_
        std::thread thread_;
    };

} // namespace tinyvm