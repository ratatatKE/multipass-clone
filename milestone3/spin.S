# spin.S - a guest that never exits and never powers off. Run it with 
#    ./tinyvm spin.bin 2000
#
# The guest executes `pause; jmp` forever. Neither instruction is sensitive, so the CPU runs them natively with NO VM exit.
# The vCPU thread is therefore stuck INSIDE the KVM_RUN system call and our run loop NEVER gets control back.
# Only the watchdog thread (main.cpp) can rescue it, by sending that thread a signal.

.code64
.text
.global _start
_start:
1:  pause
    jmp 1b
