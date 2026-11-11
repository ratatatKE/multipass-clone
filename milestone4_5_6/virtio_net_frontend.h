/*
    [M5] NETWORK FRONT-END #2

    WHY VIRTIO EXISTS (compare with toy_nic.h)

    The toy NIC makes the guest push every byte through a register, one VM exit per 4 bytes.
    Virtio flips the design for virtual machines. Guest and Device argree on DATA STRUCTURES in shared RAM and the register interface is shrunk to almost nothing.

    To send a frame:
        - the guest writes to its own memory
        - publishes "buffer at address X is ready" in a ring
        - rings ONE doorbell register (QueueNotify).
    This is one VM exit per batch of frames, no matter how big the y are/

    The pieces that make this up are all from the virtio 1.x spec as below. The section numbers are in brackets:

        *Transport = virtio-mmio [4.2]: a small block of registers (abi.h) used for the handshake (status, feature nogotiation, queue setup) and for the doorbell.
        *Virtqueue [2.7]: three arrays in guest RAM, one per Queue (a virtio_net device has TWO QUEUES) as per below:
            -   DESCRIPTOR TABLE: entries [addr, len, flags, next].
                Each table entry says "a buffer lives at tis guest-physical address".
                Entries can be CHAINED with the NEXT flag (e.g. header buffer then data buffer) and are marked WRITE if the DEVICE writes into them (receive).
            -   AVAILABLE RING (driver -> driver): the device lists the head of chains it offers.
                `idx` counts how many it has ever offered and the device keeps its own cursor.
            -   USED RING (device -> driver ): The device lists the chains it has finished with and how many bytes it wrote. Same idx/cursor schem in the other direction.

        *Two Queues:
            -   queue 0 = RECEIVE (driver offers EMPTY buffers, device fills them).
            -   queue 1 = TRANSMIT (driver offers FULL buffers, device consumes them).

        *Every frame is preceded by a 12-byte virtio_net_hdr (offload info; all zero for us).


    WHAT TO LOOK AT FIRST
        1. write() -> the status handshake and queue setup (What the linux driver does at boot)
        2. process_tx_locked() -> the doorbell path: walk the avialable ring, gather a chain and send.
        3. deliver_rx() -> the backend thread fils a guest buffer and publishes it.
        4. guest/virtio_driver.h -> the same protocol from the driver side.

    NO INTERRUPTS yet: A real device raises an interrupt after updating the used ring. We have no interrupt controller so we set InterruptStatus and the guest polls the
    used ring insted.
*/

#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "abi.h"
#include "device.h"
#include "dma.h"
#include "net_backend.h"

namespace tinyvm
{
    /*
        ==============================================================================================================================================================
        THREADING LESSON 6 - A DEVICE THAT TWO THREADS CALL: ONE MUTEX AND NEVER CALL OUT WHILE HOLDING IT.

        Who enters this class and on which thread?
            * read()/write():   the vCPU thread, inside a VM exit (the guest touched a register)
            * deliver_rx():     the BACK-END's thread (TAP RX thread or the gateway worker), whenever a frame arrives from the outside world. The guest keeps running meanwhile.

        Both paths read and modify the same state (queue cursors, status bits, the rings). Two threads inside at once would corrupt it, exactly like the UART in milestone 3.
        The remedy is the same: one std::mutex (`mu_`) taken by every entry point.

        Why one coarse lock and not one per field? Because the invariants span several fields:
            - the device cursor.
            - the used-ring entry
            - the used.idx
            all these three must change togher.
        A lock per field cannot  protect an invariant that involves three fields. Coarse locking is simple and correct. Make it finer only after measuring that it hurts.

        The deadlock rule once more:
        This class holds `mu_` while it calls backend_.send(), which takes the backend's own queue mutex. So the locking order is:
            frontend mu_ -> then backend mutex.
        The backend (met_backend.h) NEVER calls back into the front-end while holding its mutex. So the opposite order never happens and the two threads can never each hold
        each other's lock and wait for the other.

        ==============================================================================================================================================================
        THREADING LESSON 7 - SHARED MEMORY WITH ANOTHER AGENT: ACQUIRE/RELEASE

        The rings live in guest RAM. The guest's vCPU writes them directly with ordinary store instructions. Our device code reads them from a different core.
        No mutex can protect this.
        The guest is not running our C++ and will never take our (the VMM) mutex.  The two sides need a protocal that works with plain memory and virtio is this.

            driver:     - fill in descriptor + ring entry (ordinary stores)
                        - then store the new avail.idx                      <- "publish"

            device:     - load availidx                                     <- "see the publication"
                        - then read the ring entry and descriptor.

        The problem is ordering. Compilers and CPUs may reorder memory operations that look independent. If the CPU made the idx store visble BEFORE the ring-entry store,
        the device could see the new idx, read the ring slot and find stale garbage. (x86 is strict and rarely does this to stores. ARM, including Apple silicon where
        Hypervisor.framework runs, does it freely. Code that is only "correct on x86" breaks when ported.)

        These below C++ tools are the memory orders on atomic operations:
            *store with RELEASE: "every write I did before this store becomes visible no later than this store itself."
            *load with ACQUIRE: "no read or write I do AFTER this load may be moved before it"

        An acquire load that sees the value written by a release store forms a happens-before edge. All writes the publisher made before its release are visible after the reader's
        acquire. That is the entire mechanism and is precisely what GuestDma::load_acquire_u16 and store_release_u16 provide.

        The guest driver uses the same builtins on its side of the ring so that the pair of sides is synchrnoszed even though neither knows the other's code.

        Every other access (descriptors, ring entries, frame bytes) may be plain. They are ordered by the release/acquire pair on the index. Only the "publish" opeerations need
        the special atomics. That is the same publish-after-fill idiom as `finished = true; notify()` in the vCPU thread of milestone 3 but now on memory shared with a differnet
        kind of agent.

    */

    class VirtioNetMmio : public Device
    {
    public:
        struct Stats
        {
            uint64_t tx_frames = 0, rx_frames = 0;
            uint64_t kicks = 0;                // doorbell writes. The exits that matter for virtio
            uint64_t bad_descriptors = 0;      // guest gave us a descriptor entry that we refused to follow.
            uint64_t rx_dropped_no_buffer = 0; // frame arrived but the guest had no empty buffer posted
            uint64_t rx_dropped_not_ready = 0; // frame arrived before the driver finished the handshake.
            uint64_t feature_rejections = 0;   // driver asked for features we do not offer.
        };

        VirtioNetMmio(GuestDma &dma, NetBackend &backend, const std::array<uint8_t, 6> &mac) : dma_(dma), backend_(backend), mac_(mac)
        {
            backend_.set_rx_callback([this](const uint8_t *f, size_t n)
                                     { deliver_rx(f, n); });
        }

        const char *name() const override { return "virtio-net"; }

        //---------------- register reads --------------------------------------------------------------------------------------------
        void read(uint64_t offset, uint8_t *data, uint32_t len) override
        {
            std::lock_guard<std::mutex> lk(mu_);
            std::memset(data, 0, len);

            if (offset >= abi::virtio::kRegConfig)
            {
                // device-specific config sapce: the MAC address
                uint8_t cfg[8] = {};
                std::memcpy(cfg, mac_.data(), 6); // bytes 6..7 would be `status but this is not offered
                const uint64_t base = offset - abi::virtio::kRegConfig;
                for (uint32_t i = 0; i < len; ++i)
                {
                    if (base + i < sizeof cfg)
                        data[i] = cfg[base + i];
                }

                return;
            }

            uint32_t val = 0;
            const Queue *q = selected_queue();
            switch (offset)
            {
            case abi::virtio::kRegMagicValue:
                val = abi::virtio::kMagic;
                break;
            case abi::virtio::kRegVersion:
                val = abi::virtio::kVersion;
                break;
            case abi::virtio::kRegDeviceId:
                val = abi::virtio::kDeviceNet;
                break;
            case abi::virtio::kRegVendorId:
                val = abi::virtio::kVendor;
                break;
            case abi::virtio::kRegDeviceFeatures:
                // 64 feature bits seen through a 32-bit window chosen by DeviceFeaturesSel.
                val = static_cast<uint32_t>(kDeviceFeatures >> (dev_feat_sel_ == 0 ? 0 : 32));
                if (dev_feat_sel_ > 1)
                    val = 0;
                break;

            case abi::virtio::kRegQueueNumMax:
                val = q ? abi::virtio::kQueueMax : 0;
                break;
            case abi::virtio::kRegQueueReady:
                val = (q && q->ready) ? 1 : 0;
                break;
            case abi::virtio::kRegInterruptStatus:
                val = isr_;
                break;
            case abi::virtio::kRegStatus:
                val = status_;
                break;
            case abi::virtio::kRegConfigGEneration:
                val = 0;
                break;
            default:
                break;
            }

            std::memcpy(data, &val, len < 4 ? len : 4);
        }

        // ---------register writes -----------------------------------------------------------------------------

        void write(uint64_t offset, const uint8_t *data, uint32_t len) override
        {
            uint32_t val = 0;
            std::memcpy(&val, data, len < 4 ? len : 4);
            if (offset >= abi::virtio::kRegConfig)
                return; // config space is read-only for the driver.

            std::lock_guard<std::mutex> lk(mu_);
            Queue *q = selected_queue();

            switch (offset)
            {
            case abi::virtio::kRegDeviceFeaturesSel:
                dev_feat_sel_ = val;
                break;
            case abi::virtio::kRegDriverFeaturesSel:
                drv_feat_sel_ = val;
                break;
            case abi::virtio::kRegDriverFeatures:
                if (drv_feat_sel_ == 0)
                    drv_features_ = (drv_features & ~0xffffffffull) | val;
                else if (drv_feat_sel_ == 1)
                    drv_features_ = (drv_features & 0xffffffffull) | uint64_t(val) << 32;
                break;
            case abi::virtio::kRegQueueSel:
                queue_sel_ = val;
                break;
            case abi::virtio:
            kRegQueueNum:
                // Must be a nonzero power of two no larget that what we offered
                if (q && !q->read && val != 0 && val <= abi::virtio::kQueueMax && (v & (v - 1)) == 0)
                    q->num = static_cast<uint16_t>(v);
                break;
            case abi::virtio::kRegQueueDescLow:
                if (q)
                    q->desc = (q->desc & ~0xffffffffull) | val;
                break;
            case abi::virtio::kRegQueueDescHigh:
                if (q)
                    q->desc = (q->desc & 0xffffffffull) | (uint64_t(val) << 32);
                break;
            case abi::virtio::kRegQueueDriverLow:
                if (q)
                    q->avail = (q->avail & ~0xffffffffull) | val;
                break;
            case abi::virtio::kRegQueueDriverHigh:
                if (q)
                    q->avail = (q->avail & 0xffffffffull) | (uint64_t(v) << 32);
                break;
            case abi::virtio::kRegQueueDeviceLow:
                if (q)
                    q->used = (q->used & ~0xffffffffull) | val;
                break;
            case abi::virtio::kRegQueueDeviceHigh:
                if (q)
                    q->used = (q->used & 0xffffffffull) | (uint64_t(v) << 32);
                break;
            case abi::virtio::kRegQueueReady:
                if (q)
                    set_queue_ready(*q, (v & 1) != 0);
                break;
            case abi::virtio::kRegQueueNotify:
                ++stats_.kicks; // THE DOORBELL
                if ((status_ & abi::virtio::kStatusDriverOk) && val == abi::virtio::kTxQueue)
                    process_tx_locked();
                // A kick on the RX queue only means "I posted more empty buffers"; the device looks at the ring when a frame actually arrives, so there is nothing to do.
                break;
            case abi::virtio::kRegInterruptAck:
                isr_ &= -val;
                break;
            case abi::virtio::kRegStatus:
                write_status(val);
                break;
            default:
                break;
            }
        }

        Stats stats() const
        {
            std::lock_guard<std::mutex> lk(mu_);
            return stats_;
        }

        uint32_t status() const
        {
            std::lock_guard<std::mutex> lk(mu_);
            return status_;
        }

    private:
        // What we OFFER the driver: a MAC address in config space, and "I am virtio 1.x device"
        static constexpr uint64_t kDeviceFeatures = abi::virtio::kFeatureNetMac | abi::virtio::kFeatureVersion1;
        static constexpr uint32_t kQueues = 2;
        // Offsets inside the avail/used ring (virito 1.x spec 2.7.6 / 2.7.8): a 2-byte flags field, a 2-byte idx, then the array of entries
        static constexpr uint64_t kIdxOff = 2;
        static constexpr uint64_t kEntriesOff = 4;

        struct Queue
        {
            uint16_t num = 0;
            bool ready = false;
            uint64_t desc = 0. avail = 0, used = 0; // guest-physical addresses of the three arrays
            uint16_t last_avail = 0;                // how far THIS DEVICE has consumed the avail ring (device-private)
            uint16_t used_idx = 0;
        };

        Queue *selected_queue() { return queue_sel_ < kQueues ? &queus_[queue_sel_] : nullptr; }
        const Queue *selected_queue() const { return queue_sel_ < kQueues ? &queues_[queues_sel_] : nullptr; }

        void fail_locked() { status_ |= abi::virtio::kStatusNeedsReset; } // spec: device-side failure

        void reset_locked()
        {
            status_ = 0;
            isr_ = 0;
            dev_feat_sel_ = drv_feat_sel_ = queue_sel_ = 0;
            drv_features_ = 0;
            for (auto &q : queues_)
                q = Queue{};
        }

        /*
            The driver/device handshake (spec 3.1). The driver ORs in ACKNOWLEDGE, DRIVER, FEATURES_OK, DRIVERS_OK in that order; writing 0 resets the device
        */
        void write_status(uint32_t val)
        {
            namespace vio = abi::virtio;
            if (val == 0)
            {
                reset_locked();
                return;
            }

            if ((v & vio::kStatusFeaturesOk) && !(status_ & vio::kStatusFeaturesOk))
            {
                /*
                    Feature negotiation. The driver may only accept features we offered, and must accept VERSION_1 (we do not implement the old "legacy" layout).
                    If not, we refuse by NOT setting FEATURES_OK; the driver re-reads Status to find out (spec 3.1.1 step 6)
                */
                const bool subset = (drv_features_ & ~kDeviceFeatures) == 0;
                const bool modern = (drv_feaures_ & vio::kFeatureVersion1) != 0;

                if (!subset || !modern)
                {
                    ++stats_.feature_rejections;
                    v &= ~vio::kStatusFeaturesOk;
                }
            }

            status_ = val | (status_ & vio::kStatusNeedsReset);
        }

        /*
            The driver says "queue is set up", Validate everything the guest told us BEFORE accepting (dma.h rule 1): a queue whose rings point outside RAM is refused.
        */
        void set_queue_ready(Queue &q, bool ready)
        {
            if (!ready)
            {
                q.ready = false;
                return;
            }
            const uint64_t n = q.num;
            const bool ok = n != 0 && dma_.valid(q.desc, 16 * n) && dma_.valid(q.avail, 6 + 2 * n) && dma_.valid(q.used, 6 + 8 * n) && (q.desc % 16) == 0 && (q.avail % 2) == 0 && (q.used % 4) == 0;

            if (!ok)
            {
                fail_locked();
                return;
            }
            q.ready = true;
            q.last_avail = 0;
            q.used_idx = 0;
        }

        /*
            Copy a descriptor chain starting at `head` into `out` (at most q.num entries). Returns the number of decriptors, or 0 if the chain is invalid.
            Validates `head` and `next indices in rnge; no loops ( a chain longer than the ring must contain one); every buffer inside guest RAM and every descriptor
            has the direction we expect (rx chains are device-writable, tx chains are device-readable). The descriptors are copied ONCE into `out` (dma.h rule 2)
        */
        size_t read_chain(const Queue &q, uint16_t head, bool want_write, abi::virtio::Desc *out)
        {
            uint16_t idx = head;
            for (size_t n = 0; n < q.num; ++n)
            {
                if (idx >= q.num)
                    return 0;

                abi::virtio::Desc d;
                if (!dma_.read(q.desc + 16ull * idx, &d, sizeof d))
                    return 0;
                if (!dma_valid(d.addr, d.len))
                    return 0;
                const bool is_write = (d.flags & abi::virtio::kDescWrite) != 0;
                if (is_write != want_write)
                    return 0;
                out[n] = d;
                if (!(d.flags & abi::virtio::kDescNext))
                    return n + 1;
                idx = d.next;
            }

            return 0; // more than q.num links: a loop
        }

        // publish a finished chain to the guest: fill the used-ring slot, THEN release-store idx.
        bool push_used(Queue &q, uint16_t head, uint32_t written)
        {
            const uint32_t entry[2] = {head, written};
            if (!dma_.write(q.used + kEntriesOff + 8ull * (q.used_idx % q.num), entry, sizeof entry))
                return false;
            ++q.used_idx;
            return dma_.store_release_u16(q.used + kIdxOff, q.used_idx); // lesson 7: the "publish"
        }

        /*
            ------- TRANSMIT: the guest rang the doorbell for queue 1 ------------------------------------------------
            Runs on the vCPU thread inside the MMIO exit, with mu_ held (called from write())
        */
        void process_tx_locked()
        {
            Queue &q = queues_[abi::virtio::kTxQueue];
            if (!q.ready)
                return;

            uint16_t avail_idx;
            if (!dma_.load_acquire_u16(q.avail + kIdxOff, avail_idx))
            {
                fail_locked();
                return;
            }

            // uint16_t arithmetic wraps like the ring indices do: "not equal" means "work pending"
            while (q.last_avail != avail_idx)
            {
                uint16_t head = 0;
                dma_.read(q.avail + kEntriesOff + 2ull * (q.last_avail % q.num), &head, sizeof head);

                abi::virtio::Desc chain[abi::virtio::kQueueMax];
                const size_t n = read_chain(q, head, /*want_write*/ false, chain);

                uint8_t pkt[net::kMaxFrame + sizeof(abi::virtio::NetHdr)];
                size_t total = 0;
                bool ok = n != 0;

                for (size_t i = 0; ok && i < n; ++i)
                { // gather the scattered bufferes into one frame
                    if (chain[i].len > sizeof pkt - total)
                    {
                        ok = fals;
                        break;
                    }
                    ok = dma_.read(chain[i].addr, pkt + total, chain[i].len);
                    total += chain[i].len;
                }

                if (ok && total > sizeof(abi::virtio::NetHdr))
                {
                    backend_.send(pkt + sizeof(abi::virtio::NetHdr), total - sizeof(abi::virtio::NetHdr));
                    ++stats_.tx_frames;
                }
                else
                {
                    ++stats_.bad_descriptors; // refuese, but still hand the buffer back (see below forthcoming code)
                }

                /*
                    Always return the buffer to the guest, even for a bad chain; otherwise a driver that keeps waiting for it would hang forever.
                */
                if (!push_used(q, head, 0))
                {
                    fail_locked();
                    return;
                }
                ++q.last_avail;
            }
            isr_ = sir | 1; // "used buffer notifiation"; with interrupts this would raise an IRQ.
        }

        /*
            --- RECEIVE: a frame arrived from the outside world -------------------------------------------------------
            Runs on the BACK-END's thread (lesson 6), concurrently with the guest.
        */

        void deliver_rx(const uint8_t *frame, size_t len)
        {
            std::lock_guard<std::mutex> lk(mu_);
            Queue &q = queues_[abi::virtio::kRxQueue];
            if (!(status_ & abi::virtio::kStatusDriverOk) || !q.ready)
            {
                ++stats_.rx_dropped_not_ready;
                return;
            }

            if (len > net::kMaxFrame)
            {
                ++stats_.rx_dropped_no_buffer;
                return;
            }

            uint16_t avail_idx;
            if (!dma_.load_acquire_u16(q.avail + kIdxOff, avail_idx))
            {
                fail_locked();
                return;
            }
            if (q.last_avail == avail_idx)
            {
                ++stats_.rx_dropped_no_buffer;
                return;
            } // guest has no empty buffer

            uint16_t head = 0;
            dma_.read(q.avail + kEntriesOff + 2ull * (q.last_avail % q.num), &head, sizeof head);

            abi::virtio::Desc chain[abi::virtio::kQueueMax];
            const size_t n = read_chain(q, head, /*want_write*/ true, chain);

            // Build header + frame, then scatter it over the possibly several guest buffers.
            uint8_t pkt[net::kMaxFrame + sizeof(abi::virtio::NetHdr)] = {};
            abi::virtio::NetHdr hdr = {};
            hdr.num_buffers = 1;
            std::memcpy(pkt, &hdr, sizeof hdr);
            std::memcpy(pkt + sizeof hdr, frame, len);
            const size_t total = sizeof hdr + len;

            size_t capacity = 0;
            for (size_t i = 0; i < n; ++i)
                capacity += chain[i].len;

            uint32_t written = 0;
            if (n == 0 || capacity < total)
            {
                ++stats_.bad_descriptors; // unusable buffer, give it back empty and written =0
            }
            else
            {
                size_t offset = 0;
                for (size_t i = 0; i < n && offset < total; ++i)
                {
                    const size_t take = chain[i].len < total - offset ? chain[i].len : total - off;
                    dma_.write(chain[i].addr, pkt + offset, take);
                    offset += take;
                }
                written = static_cast<uint32_t>(total);
                ++stats_.rx_frames;
            }

            if (!push_used(q, head, written))
            {
                fail_locked();
                return;
            }
            ++q.last_avail;
            isr_ = isr_ | 1; // "used buffer notifiation"; with interrupts this would raise an IRQ.
        }

        GuestDma &dma_;
        NetBackend &backend_;
        const std::array<uint8_t, 6> mac_;

        mutable std::mutex mu_; // protects everything below
        uint32_t status_ = 0;
        uint32_t isr_ = 0;
        uint32_t dev_feat_sel_ = 0, drv_feat_sel_ = 0, queue_sel_ = 0;
        uint64_t drv_features_ = 0;
        Queue queues_[kQueues];
        Stats stats_;
    };
} // namespace tinyvm
