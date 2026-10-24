# guest.S - milestone 2 guest. 32-bit flat protected mode, loaded at guest-physical 0x1000.
#
# Why 32-bit instead of milestone 1's 16-bit real mode? Our MMIO device lives at
# 0xd0000000, far above 1 MiB, and real mode can't address that. The hypervisor puts the
# vCPU straight into flat 32-bit protected mode (see Vcpu::reset_to_protected_mode).
#
# What it does, and which kind of VM exit each step causes:
#   1. print text through the UART       out/in on ports 0x3f8/0x3fd -> KVM_EXIT_IO
#   2. read the control device ID        load from 0xd0000000        -> KVM_EXIT_MMIO (read)
#   3. read its access counter 3 times   loads from 0xd0000004       -> KVM_EXIT_MMIO (read)
#   4. write + read back its scratch reg 0xd000000c                  -> KVM_EXIT_MMIO (write, read)
#   5. write + read back normal RAM      store/load to `scratch`     -> NO exit (hardware does it)
#   6. read an address nobody owns       load from 0xdead0000        -> KVM_EXIT_MMIO, open bus
#   7. write an exit code to POWEROFF    store to 0xd0000008         -> KVM_EXIT_MMIO (write)
 
 .code32
 .text
 .global _start

 .set UART_DATA,        0x3f8
 .set UART_LSR,         0x3fd
 .set CTL_BASE,         0xd0000000
 .set CTL_ID,           0x00
 .set CTL_COUNT,        0x04
 .set CTL_POWEROFF,     0x08
 .set CTL_SCRATCH,      0x0c

 _start:
    mov $0x90000, %esp                  # a stack inside our 1 MiB of RAM

    mov $s_banner, %esi
    call puts

    # --- 2. MMIO read: device ID ---
    mov $CTL_BASE, %ebx                 # ebx = base of the control device, kept for the whole program. That's why all our CTL_* variables henceforth are done using relative addressing to %ebx
    mov $s_id, %esi
    call puts
    mov CTL_ID(%ebx), %edi
    call print_hex32
    call newline

    # --- 3. MMIO reads: the access counter goes up on every access ---
    mov $s_count, %esi                  # s_count will be the variable we are getting from C++ for tracking counts.
    call puts
    mov $3, %ecx
1:  mov CTL_COUNT(%ebx), %edi
    call print_hex32
    mov $0x20, %al
    call putc
    loop 1b
    call newline

    # --- 4. MMIO write then read: the device keeps state ---
    mov $s_scratch, %esi
    call puts
    movl $0x12345678, CTL_SCRATCH(%ebx)
    mov CTL_SCRATCH(%ebx), %edi
    call print_hex32
    call newline

    # --- 5. Plain RAM: same shape of code, but no VM exit at all ---
    mov $s_ram, %esi
    call puts
    movl $0xcafebabe, scratch
    mov scratch, %edi
    call print_hex32
    call newline

    # --- 6. Unmapped MMIO: nobody answers, the hypervisor returns all-ones ---
    mov $s_open, %esi
    call puts
    mov 0xdead000, %edi
    call print_hex32
    call newline

    # --- 7. Power off with exit code 7 ---
    mov $s_bye, %esi
    call puts
    movl $7, CTL_POWEROFF(%ebx)
1:  hlt                                         # not reached; the hypervisor stops the VM first
    jmp 1b

# putc: al = character. Polls the UART line-status register until it is ready and then sends.
# Every character therefore costs two VM exits: one `in`, one `out`
putc:
    push %edx
    push %eax
    mov $UART_LSR, %dx
1:  in %dx, %al
    test $0x20, %al                             # LSR bit 5: transmit holding register empty
    jz 1b
    pop %eax                                    # restore the character
    mov $UART_DATA, %dx
    out %al, %dx
    pop %edx
    ret

# puts: esi -> NUL-terminated string
puts:
    push %eax
1:  lodsb
    test %al, %al
    jz 2f
    call putc
    jmp 1b
2:  pop %eax
    ret

newline:
    push %eax
    mov $0x0a, %al
    call putc
    pop %eax
    ret

# print_hex32: edi = value, prints 8 lowercase hex digits
print_hex32:
    push %eax
    push %ecx
    push %edx
    mov $8, %ecx
1:  rol $4, %edi                                # bring the next nibble to the bottom
    mov %edi, %edx
    and $0xf, %edx
    mov hexdigits(, %edx, 1), %al
    call putc
    loop 1b
    pop %edx
    pop %ecx
    pop %eax
    ret

hexdigits:      .ascii  "0123456789abcdef"
scratch:        .long 0

s_banner:       .asciz  "tinyvm m2: guest is up (32-bit protected mode) \n"
s_id:           .asciz  "control device ID              :0x"
s_count:        .asciz  "access counter x3              :0x"
s_scratch:      .asciz  "scratch reg round trip         :0x"
s_ram:          .asciz  "plain RAM round trip           :0x"
s_open:         .asciz  "unmapped read (0xdead0000)     :0x"
s_bye:          .asciz  "powering off with code 7 \n"

