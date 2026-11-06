/*
x86-64 page tables, built and walked in software.

The three address sapaces in play (this is the "memory" topic of my interview list):

    guest-virtual (GVA)     what the guest's own instructions use. Translated by the GUEST's page tables, which live in guest RAM
                            and are pointed to by CR3.
    
    guest-physical (GPA)    what the guest believes is "real" memory. This is translated by KVM's memslots + the 
                            CPU's second-level tables (EPT on Intel and NPT on AMD)

    host-virtual    (HVA)   our mmaped()ed buffer in this process. The memslot says "GPA x is the same byte as HVA y".

    host-physical   (HPA)   the real DRAM address the host kernel chose for that page of our mmap.

Hardware does GVA -> GPA -> HPA on every guest memory access ("a 2D page walk").
The VMM never sees HPA directly, it only knows HVA and the host kernel is free to move the page behind it.
This file, paging.h, lets us have visibility at every step

*/
#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>

#include "kvm_util.h"

namespace tinyvm {
// --- x86_64 page-table-entry bits (identical format at all four levels, mostly) ---------
constexpr uint64_t kPtePresent  = 1ull << 0;     // 0 => the CPU faults (#PF); bits 1..63 are then free for the OS
constexpr uint64_t kPteWritable = 1ull << 1;
constexpr uint64_t kPteUser     = 1ull << 2;
constexpr uint64_t kPtePwt      = 1ull << 3;    // page write-through   } together with page-cache disable i.e. PCD, select the
constexpr uint64_t kPtePcd      = 1ull << 4;    // page cache disable   } memory type, MMIO wants the type to be UNcached.
constexpr uint64_t kPteLarge    = 1ull << 7;    // "PS": in a page-DIRECTORY entry, maps 2 MiB directly.
constexpr uint64_t kPteAddrMask = 0x000ffffffffff000ull;    // bits 12..51: physical address of the next table/page

constexpr uint64_t kPageSize        = 4096;
constexpr uint64_t kLargePageSize   = 2ull << 20;

// -- below is the guest memory layout used by main.cpp, guest.S and the tests ------------
constexpr uint64_t kTablesGpa   = 0x10000;          // page tables are allocated from here upwards
constexpr uint64_t kDataGpa     = 0x20000;          // a page of plain data in RAM
constexpr uint64_t kAliasGva    = 0x200000;         // virtual address that ALSO maps to kDataGpa
constexpr uint64_t kHoleGva     = 0x201000;         // next page after the alias: page table entry left empty
constexpr uint64_t kFarHoleGva  = 0x400000;         // page DIRECTORY entry left empty
constexpr uint64_t kMmioGpa     = 0xd0000000;       // the ControlDevice, mapped 1:1 as one uncached 2MiB page

/*
    Builds a 4-level tree: PML4 -> PDT -> PD -> PT. Each table is one 4 KiB page of 512 8-byte entries since each level consumes 9 bits of the
    virtual address in x86.

    63..48  47..39   38..30   29..21   20..12   11..0
    (sign)  PML4 idx PDPT idx PD idx   PT idx   offset in the 4 KiB page
*/

class PageTables {
public:
    PageTables(GuestMemory& mem, uint64_t first_table_gpa): mem_(mem), next_(first_table_gpa) {
        root_ = alloc_table();
    }

    uint64_t cr3() const { return root_; }      // the value that the guest's CR3 register must hold

    void map_4k(uint64_t va, uint64_t pa, uint64_t flags){
        require_aligned(va, kPageSize);
        require_aligned(pa, kPageSize);
        const uint64_t pdpt = descend(root_, index(va, 39));
        const uint64_t pd   = descend(pdpt, index(va, 30));
        const uint64_t pt   = descend(pd, index(va, 21));           // throws if a 2 MiB page is already here
        set(pt, index(va, 12), pa | flags | kPtePresent);
    }

    void map_2m(uint64_t va, uint64_t pa, uint64_t flags){
        require_aligned(va, kLargePageSize);
        require_aligned(pa, kLargePageSize);
        const uint64_t pdpt = descend(root_, index(va, 39));
        const uint64_t pd   = descend(pdpt, index(va, 30));
        const uint64_t old  = get(pd, index(va, 21));
        if ((old & kPtePresent) && !(old & kPteLarge))
            throw std::invalid_argument("paging: range already has a 4 KiB page table");
        
        set(pd, index(va, 21), pa | flags | kPtePresent | kPteLarge);
    }

    size_t tables_used() const { return (next_ - tables_start_) / kPageSize; }

private:
    static uint64_t index(uint64_t va, unsigned shift) {
        return (va >> shift) & 0x1ff;
    }

    static void require_aligned(uint64_t v, uint64_t align){
        if(v & (align - 1)) throw std::invalid_argument("paging: address not aligned");
    }

    uint64_t alloc_table(){
        if(next_ + kPageSize > mem_.size())     throw std::runtime_error("paging: out of guest RAM (OOM) for page tables");
        const uint64_t gpa = next_;
        next_ = next_ + kPageSize;
        std::memset(mem_.host_ptr(gpa), 0, kPageSize); // all entries "NOT PRESENT" for this PAGE
        return gpa;
    }

    uint64_t get(uint64_t table, uint64_t idx) const {
        uint64_t e;
        std::memcpy(&e, mem_.host_ptr(table + idx * 8), sizeof e);
        return e;
    }

    void set(uint64_t table, uint64_t idx, uint64_t e){
        std::memcpy(mem_.host_ptr(table + idx * 8), &e, sizeof e);
    }

    // Follow (or create) the entry `idx` of `table`, returning the next-level table's GPA
    uint64_t descend(uint64_t table, uint64_t idx){
        const uint64_t e = get(table, idx);
        if( e & kPtePresent){
            if ( e & kPteLarge)     throw std::invalid_argument("paging: range already mapped by a large page");
            return e & kPteAddrMask;
        }

        const uint64_t child = alloc_table();
        set(table, idx, child | kPtePresent | kPteWritable);
        return child;
    }

    GuestMemory& mem_;
    uint64_t next_;
    uint64_t root_ = 0;
    const uint64_t tables_start_ = next_;
};

/*
    The result of a software page walk.
*/
struct Walk {
    bool present = false;       // true if the translation succeeded
    uint64_t gpa = 0;           // the guest-physical address (valid if present)
    int fault_level = 0;        // if !present: 4 = PML4 entry missing ... 1 = PT entry missing
    bool writable = false;      // AND of the writable bits on the path (what the CPU would check)
};

/*
    This function does by hand exactly what the CPU's page-walk hardware does:
    - start at CR3,
    - use 9 bits of the address per level to pick an 8 byte entry,
    - stop at a non-present entry (=> #PF) or a leaf (4 KiB page at level 1 or 2 MiB page at level 2 when the PS bit is set).
*/

inline Walk walk(const GuestMemory& mem, uint64_t cr3, uint64_t va){
    Walk w; // instantianted on stack
    w.writable = true;
    uint64_t table = cr3 & kPteAddrMask;

    for(int level=4; level>=1; --level){
        const unsigned shift = 12 + 9 * (level - 1);
        const uint64_t idx = (va >> shift) & 0x1ff;

        if(table + kPageSize > mem.size()) { w.fault_level = level; return w; }
        uint64_t e;
        std::memcpy(&e, mem.host_ptr(table + idx * 8), sizeof e);
        if(!(e & kPtePresent)) { w.fault_level = level; return w; }
        w.writable = w.writable && (e & kPteWritable);
        if(level == 2 && (e & kPteLarge)){
            w.present = true;
            w.gpa = (e & 0x000fffffffe00000ull) | (va & (kPageSize - 1));
            return w;
        }
        if(level == 1){
            w.present = true;
            w.gpa = (e & kPteAddrMask) | (va & (kPageSize -1));
            return w;
        }

        table = e & kPteAddrMask;
    }

    return w; // unreachable i.e. should never be reached.
}


// The one page-table layout every part of this milestone shares
inline uint64_t build_guest_tables(GuestMemory& mem){
    PageTables pt(mem, kTablesGpa);

    /*
        Identity-map all of RAM with 4 KiB pages: VA == PA. The guest needs this so that once paging is on, its own code, stack and page
        tables remain reachable at the same numbers.
    */
   for (uint64_t a=0; a < 0x100000; a = a + kPageSize)  pt.map_4k(a, a, kPteWritable);

   // A second virtual name for the data page: VA 0x200000 -> PA 0x20000.
   pt.map_4k(kAliasGva, kDataGpa, kPteWritable);

   /*
        The control device: one 2 MiB page, identity-mapped, caching disabled (MMIO must never be served from the CPU cache, 
        or a read could return a stale device value).
   */
  pt.map_2m(kMmioGpa, kMmioGpa, kPteWritable | kPtePcd | kPtePwt);
  return pt.cr3();
}

/*
    Ask the host kernel which physical page backs a host-virtual address of ours.
    /proc/self/pagemap has one 8-byte entry per virtual page: bit 63=present, bits 0...54 = page frame number.

    Since Linux 4.0, the frame number reads as 0 unless you are root (i.e. CAP_SYS_ADMIN) to make Rowhammer-style attacks harder.
    Note: Inside a VirtualBox CM, this is "physical" as seen by the Ubuntu VM i.e. itself a guest-physical address of the level below.
    Nested virtualization stacks these translations.
*/

inline std::optional<uint64_t> host_physical(const void* p){
    const int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if(fd < 0) return std::nullopt;
    const uint64_t va = reinterpret_cast<uintptr_t>(p); // change the pointer to an unsigned integer.
    uint64_t entry = 0;
    const ssize_t n = pread(fd, &entry, sizeof entry, static_cast<off_t>((va / kPageSize) * 8));
    close(fd);
    if(n != static_cast<ssize_t>(sizeof entry)) return std::nullopt;
    if(!(entry >> 63)) return std::nullopt;                             // page not resident
    const uint64_t pfn = entry & ((1ull << 55) - 1);                    // pfn==page frame number?
    if(pfn==0) return std::nullopt;                                     // hidden: not root
    return pfn * kPageSize + (va & (kPageSize-1));
}


} // namespace tinyvm






























