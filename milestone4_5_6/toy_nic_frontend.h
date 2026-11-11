/*
    
    [M4] NETWORK FRONT-END #1 

    toy_nic.h - A toy, old-style network card.

    This is the kind of NIC that predates virtualization (think NE2000, or a UART with a bigger FIFO).

    The CPU is the data mover. To send a frame, the driver:
        1. Writes the length to TX_LEN.
        2. Writes the frame 4 bytes at a time to TX_DATA.
        3. Writes TX_GO.

    To receive, the driver polls STATUS, reads RX_LEN, reads RX_DATA 4 bytes at a time, writes RX_POP registers.

    In real hardware, that is slow because every register access crosses the bus. In a VM, it is even worse because every access is a VM exit
    that costs microseconds. A 60-byte ping costs about 18 exits to send and 18 other exits to receive, plus one exit for every time the driver polls
    the status registor while waiting.

    Run the demo with --nic=toy and then --nic=virtio and compare the "exits per frame" line in the report. That comparison is the reason virtio exists.

    This front-end knows nothing about TAP or gateways, it only talks to a NetBackend (some sort of interface).
*/

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "abi.h"
#include "device.h"
#include "net_backend.h"

namespace tinyvm {

    class ToyNic: public Device {
        public:
            struct Stats {
                uint64_t tx_frames = 0, rx_frames = 0, rx_dropped = 0, tx_bad = 0;
            };

            ToyNic(NetBackend& backend, const std::array<uint8_t, 6>& mac) : backend_(backend), mac_(mac){
                // From now on the backend's thread may call us at any moment once it is started.
                backend_.set_rx_callback([this](const uint8_t* f, size_t n){
                    on_backend_frame(f, n);
                });
            }

            const char* name() const override { return "toy-nic"; }

            /*
                ---- guest reads a register (runs on the vCPU thread) inside a VM exit --------------
            */

            void read(uint64_t offset, uint8_t* data, uint32_t len) override {
                std::lock_guard<std::mutex>lk(mu_);
                uint32_t val = 0;

                switch(offset){
                    case abi::toy::kRegMagic: val = abi::toy::kMagic; break;
                    case abi::toy::kRegStatus:
                        val = abi::toy::kStatusLinkUp | (rx_.empty() ? 0 : abi::toy::kStatusRxReady);
                        break;
                    case abi::toy::kRegMacLo: std::memcpy(&val, mac_.data(), 4); break;
                    case abi::toy::kRegMacHi: std::memcpy(&val, mac_.data() + 4, 2); break;
                    case abi::toy::kRegRxLen: val = rx_.empty()? 0 : static_cast<uint32_t>(rx_.front().size()); break;
                    case abi::toy::kRegRxData:
                        // A read with a SIDE EFFECT:  it consumes the next 4 byres of the frame. This is why device registors are NOT MEMORY.
                        if(!rx_.empty()){
                            const auto& f = rx_.front();
                            for(size_t i=0; i<4 && rx_cursor_ + i < f.size(); ++i)
                                val = val | static_cast<uint32_t>(f[rx_cursor_ +  i]) << (8 * i);
                            rx_cursor_ +=4;
                        }
                        break;
                    
                    default: break;
                }

                std::memset(data, 0, len);
                std::memcpy(data, &val, len < 4? len : 4);
            }


            //---- guest writes a register -----------------------------------------------------------
            void write(uint64_t offset, const uint8_t* data, uint32_t len) override {
                uint32_t val = 0;
                std::memcpy(&v, data, len < 4 ? len : 4);

                std::lock_guard<std::mutex> lk(mu_);
                switch(offset){
                    case abi::toy::kRegTxLen:
                        tx_expected_ = val <= net::kMaxFrame? val : 0;  // 0 = refuse oversized frames
                        tx_.clear();
                        break;
                    
                    case abi::toy::kRegTxData:
                        for(size_t i=0; i<4 && tx_.size() < tx_expected_; ++i)
                            tx_.push_back(static_cast<uint8_t>( v >> (8 * i)));
                        break;

                    case abi::toy::kRegTxGo:
                        if(tx_expected_ != 0 && tx_.size() == tx_expected_){
                            /*
                               Lock ordering
                               first front-end mutex (held) -> then backend's queue mutex (taken inside send)

                               the backend never calls us while holding it's OWN mutex, so this order is the only one that exists and there is never a deadlock.
                            */
                           backend_.send(tx_.data(), tx_.size());
                           ++stats_.tx_frames;
                        } else [
                            ++stats_.tx_bad;
                        ]
                        tx_.clear();
                        tx_expected_ = 0;
                        break;
                
                    case abi::toy::kRegRxPop:
                        if(!rx_.empty()) rx_.pop_front();
                        rx_cursor_ = 0;
                        break;
                    
                    default: break;
                }
            }
        
        Stats stats() const {
            std::lock_guard<std::mutex> lk(mu_);
            return stats_;
        }

        private:
            /*
                ---- THE BACK-END's THREAD ENTERS HERE ---------------------------------------------------
                This piece of code runs on the backend's RX thread while the VCPU thread may be inside read()/write() above.
                Both threads touch rx_, so both take mu_. That is the whole synchronization story.
            */

            void on_backend_frame(const uint8_t* frame, size_t len){
                std::lock_guard<std::mutex> lk(mu_);
                if(rx_.size() >= kRxQueueMax || len > net::kMaxFrame){
                    ++stats_.rx_dropped;    // a real NIC also drops when its buffer is full;
                    return;
                }
                rx_.emplace_back(frame, frame + len);
                ++stats_.rx_frames;
            }

            static constexpr siz_t kRxQueueMax = 16;
            
            NetBackend& backend_;
            const std::array<uint8_t, 6> mac_;

            mutable std::mutex mu_;                                 // protects everything below
            std::vector<uint8_t> tx_;
            uint32_t tx_expected_ = 0;
            std::deque<std::vector<uint8_t>> rx_;
            size_t rx_cursor_ = 0;                                  // how many bytes of rx_.front() were already read
            Stats stats_;
    };



} // namespace tinyvm

