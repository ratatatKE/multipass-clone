/*

threads_demo.cpp = Threading lessons 1 and 2. This file is runnable without KVM using below commands:
    make threads_demo && ./threads_demo
    make disassm-demo                   # Shows the machine code that makes the race possible

Here is how the demo works:

Four threads each add 1 to the same counter two million times. The correct answer should be 8,000,000. 
We do it three ways and print what we actually got.


--- WHY THE FIRST VERSION LOSES COUNTS ------------
`++counter` on a variable  in memory is not one operation as per the perspective of the machine. It is:

    mov counter(%rip), %eax             : 1. LOAD the current value into a register.
    add $1, %eax                        : 2. ADD in the register
    mov %eax, counter(%rip)             : 3. STORE the result back

(compilers sometimes fuse these two into `addl $1, counter(%rip)`, but inside the core that is still load, add, store.). 
Two cores running the above instructions can interleave like below:

    core A: load -> 41
    core B: load -> 41
    core A: add, store 42
    core B: add, store 42           <- Notice that B overwrote A's work: two increments and teh counter rose by only 1.

Each core has its own private caches; a store becomes visible to the other cores only after the cache-coherence protocol (MESI) has moved the  
cache-line arond. Nothing makes the 3-step sequence indivisible unless WE ask for it.

The racy version below uses `volatile` only to stop the compiler from optimising and collapsing teh single loop into a single `counter +=2000000`
(which would hide the bug). `volatile` means "really perform every load and store" and does NOT mean "indivisible" and is not a threading tool.
The C++ standard still calls this a data race with undefined behaviour, so the number you see is whatevr the hardware happened to do on this run 
and it can change.

NOTE: on a truly single core, the threads take turns instead of overlapping, so a thread can only be interrupted between its load and its store 
when the scheduler happens to preempt it right there, and teh race is rarer. 
(NOTE that containers may report 1 hardware thread yet still run several in parallel, the run that was checked by AI lost over 40% of the increments).
A run with no losses would not prove that the code is correct, it only proves that the hardware hid the bug. ThreadSanitizer finds such races 
without needing them to strike. You can run thread sanitizer with `make tsan`.

--- VERSION 2: std::atomic --------------------
fetch_add() in C++ atomics compiles to .asm `lock xadd` ( or plain `lock_add` when, as here, nobody looks at the old value. Confirm this with
`make disasm-demo`). The `lock` prefix makes the core hold the cache line in exclusive state for the whole load-add-store, so that the 
three steps become indivisible. 
Cost? - When several cores fight over the same line, it ping-pongs between their caches, each transfer taking thens of nanoseconds.

--- VERSION 3: std::mutex --------------------
Correct, and the right tool when the protected region is more than one variable e.g. the UART's state. 
Uncontended, lock() in C++  is one `lock cmpxchg` and unlock is `xchg`, no system call. 
Contended, a loser in C++ calls futex(FUTEX_WAIT) syscall and the kernel puts the loser to sleep. 
The unlocker would then call futex(FUTEX_WAKE). That sleep/wake costs microseconds, which is why a mutex is far slower than an atomic under
heavy contention for a trivial critical section.

*/

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace{

constexpr int kThreads = 4;
constexpr int kIters = 2'000'000;

volatile int racy_conter = 0;           // lesson 1: shared, unprotected
std::atomic<int> atomic_counter { 0 };  // lesson 2
int locked_counter = 0;                 // lesson 3: plain int, protected by mutex `mu`
std::mutex mu;

// noinline keeps the loop body  visible in the disassembly (see `make disasm-demo`).
__attribute__((noinline)) void racy_worker(){
    for (int i =0 ; i < kIters; ++i)    racy_counter = racy_conter + 1; // load, add, store
}

__attribute__((noinline)) void atomic_worker(){
    for (int i=0; i< kIters; ++i) atomic_counter.fetch_add(1);          // lock xadd
}

__attribute__((noinline)) void locked_worker() {
    for(int i=0; i<kIters; ++i){
        std::lock_guard<std::mutex> lk(mu); // lock on entry, unlock at the end of each iteration.
        ++locked_counter;
    }
}

template <typename Func>
double run(Func worker){
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for(int i =0; i<kThreads; ++i) threads.emplace_back(worker);
    for (auto& t : threads) t.join(); // never leave a joinable std::thread to be destroyed when it exits scope.
    return std::chrono::duration<double, std::mill>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace



int main(){
    const int expected = kThreads * kIters;
    std::printf("%d threads x %d increments; hardware threads available: %u\n\n", kThreads, kIters, std::thread::hardware_concurrency());

    double ms = run(racy_worker);
    std::printf("volatile int: %9d (expected %d) lost %d %.0f ms <- data race \n", static_cast<int>(racy_counter), expected, expected - static_cast<int>(racy_counter), ms);

    ms = run(atomic_worker);
    std::printf("std::atomic    : %9d (expected %d) lost %d %.0f ms\n", atomic_counter.load(), expected, expected - atomic_counter.load(), ms);

    ms = run(locked_worker);
    std::printf("std::mutex     : %9d (expected %d) lost %d %.0f ms\n", locked_counter, expected, expected - locker_counter, ms);
    

    return 0;

}





















