/*
   test_m3.cpp = milestone 3 tests for paging.h and for the UART's thread safety. No KVM needed. Run it with
        `make test`     does a plain run
        `make tsan`     the same tests under ThreadSanitizer, which watches every memory accesss and reports data races;
                        removing the lock_guards in devices.h makes it fail loudly and terribly.
*/

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "devices.h"
#include "kvm_util.h"
#include "paging.h"

using namespace tinyvm;

static int failures = 0;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if(!(cond)){                                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s", __FILE__, __LINE__, #cond);          \    
            ++failures;                                                                 \
        }                                                                               \
    } while(0)                    
    
template <typename F>
static bool throws(F f) {
    try { f(); } catch(const std::exception&){ return true; }
    return false;
}


static void test_layout_walks(){
    GuestMemory ram(1<<20);
    const uint64_t cr3 = build_guest_tables(ram);
    CHECK(cr3 == kTablesGpa);

    // Identity-mapped RAM
    Walk w = walk(ram, cr3, 0x1234);
    CHECK(w.present && w.gpa == 0x1234 && w.writable);
    w = walk(ram, cr3, 0xff000 + 0xabc);
    CHECK(w.present && w.gpa == 0xff000 + 0xabc);

    // The alias: virtual 0x200000 and physical 0x20000 are the same page; offsets carry through.
    w = walk(ram, cr3, kAliasGva);
    CHECK(w.present && w.gpa == kDataGpa);
    w = walk(ram, cr3, kAliasGva + 0x123);
    CHECK(w.present && w.gpa == kDataGpa + 0x123);

    // Holes fail at the level where the first non-present entry sits.
    w = walk(ram, cr3, kHoleGva);
    CHECK(!w.present && w.fault_level == 1);            // PT exists, entry empty
    w = walk(ram, cr3, kFarHoleGva);
    CHECK(!w.present && w.fault_level == 2);            // PD entry empty
    w = walk(ram, cr3, 0x80000000ull);
    CHECK(!w.present && w.fault_level == 3);            // PML4 entry exists, PDPT entry for 2 GiB is empty
    w = walk(ram, cr3, 0xffff800000000000ull);
    CHECK(!w.present && w.fault_level == 4);            // PML4 entry empty

    // The MMIO 2 MiB page: offset within the large page is preserved.
    w = walk(ram, cr3, kMmioGpa);
    CHECK(w.present && w.gpa == kMmioGpa);
    w = walk(ram, cr3, kMmioGpa + 0x1fffff);
    CHECK(w.present && w.gpa == kMmioGpa + 0x1fffff);
    w = walk(ram, cr3, kMmioGpa + 0x200000);            // next 2 MiB: not mapped
    CHECK(!w.present);
}


static void test_builder_rules(){
    GuestMemory ram(1<<20);
    PageTables pt(ram, kTablesGpa);
    pt.map_4k(0x5000, 0x6000, kPteWritable);
    CHECK(walk(ram, pt.cr3(), 0x5010).gpa == 0x6010);

    CHECK(throws([&]{pt.map_4k(0x5001, 0x6000, 0); }));             // misaligned VA
    CHECK(throws([&]{pt.map_4k(0x5000, 0x6001, 0); }));             // misaligned PA
    CHECK(throws([&]{pt.map_2m(0x100000, 0x100000, 0); }));         // 1 MiB is not 2 MiB aligned

    pt.map_2m(0x400000, 0x400000, kPteWritable);
    CHECK(throws([&]{ pt.map_4k(0x401000, 0x1000, 0); }));          // inside a large page
    pt.map_4k(0x200000, 0x1000, 0);                                 // different PD entry; fine
    CHECK(throws([&]{ pt.map_2m(0x200000, 0x200000, 0); }));        // over an existing PT

    // Read-only is repported as such by the walker
    PageTables pt2(ram, kTablesGpa + 0x10000);
    pt2.map_4k(0x8000, 0x8000, 0);                                  // no kPteWritable
    CHECK(walk(ram, pt2.cr3(), 0x8000).present);
    CHECK(walk(ram, pt2.cr3(), 0x8000).writable);
}

static void test_out_of_table_memory_ie_OOM(){
    GuestMemory ram(0x4000);                                        // room for only 4 tables
    PageTables pt(ram, 0);                                          // root takes one
    pt.map_4k(0x1000, 0x1000, 0);                                   // PDPT, PD, PT take 3 more, exactly full
    CHECK(throws([&]{ pt.map_4k(0x40000000ull, 0x2000, 0); }));     // needs a new table, none left
}


/*
    Four threads hammer one URAT. With the mutex, every byte is counted and written exactly once.
    (Try deleting the lock_guards in devices.h, the counter will come up short or TSan complains)
*/
static void test_uart_is_thread_safe(){
    constexpr int kThreads = 4;
    constexpr int kBytesEach = 5000;
    std::ostringstream out;
    Uart16550 uart(out);

    std::vector<std::thread> threads;
    for(int t=0; t<kThreads; ++t){
        threads.emplace_back(
            [&uart, t]{
                const uint8_t ch = static_cast<uint8_t>('a' + t);
                for(int i=0; i<kBytesEach; ++i){
                    uart.write(0, &ch, 1);
                    uint8_t lsr = 0;
                    uart.read(5, &lsr, 1);
                }
            }
        );
    }

    // Reading from yet another thread while the writers run is also safe.
    uint64_t seen = 0;
    for (int i=0; i<100; ++i) seen=uart.tx_bytes();

    (void)seen;
    for(auto& t: threads) t.join();

    CHECK(uart.tx_bytes() == static_cast<uint64_t>(kThreads) * kBytesEach);
    CHECK(out.str().size() == static_cast<size_t>(kThreads) * kBytesEach);

    for(int t=0; t < kThreads; ++t){
        size_t n = 0;
        for (char c : out.str()) n = n + (c=='a' + t);
        CHECK(n== static_cast<size_t>(kBytesEach));
    }
}

int main(){
    test_layout_walks();
    test_builder_rules();
    test_out_of_table_memory_ie_OOM();
    test_uart_is_thread_safe();

    if(failures == 0) std::printf("m3 tests passed \n");
    else std::printf("%d check(s) failed \n", failures);
    return failures == 0 ? 0 : 1;
}



























