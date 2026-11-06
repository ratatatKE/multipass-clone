# guest.S - milstone 3 guest. 64-bit LONG mode with paging ON loaded at guest-physical address = 0x1000
#
# (See main.cpp) By the time this code runs, the hypervisor has already:
#   - built page tables in guest RAM and put their address in CR3.
#   - set CR4.PAE, EFER.LME + EFER.LMA, CR0.PE + CR0.PG -> 64-bit mode, paging enabled.
# so every address this code uses is a GUEST-VIRTUAL address that the CPU actually translates.
#
# What this code shows in this file (guest.S) step by step:
#   1. read CR3 / CR0 / EFER                            to prove that paging and long mode are really on.
#   2. write via VA 0x200000, read via 0x20000          two virtual names point to the same physical page.
#   3. MMIO through a virtual address                   VA 0xd0000000 is mapped (uncached) to the device.
#   4. power off                                        store to the device's POWEROFF register
#
# Exits worth noticing:
#   - steps 1-2 above cause NO exit to userspace because MOV from CR, loads and stores to RAM are handled by hardware.
#   - step 3 causes a VM exit because the guest page tables say "OK, physical address 0xd0000000",
#       but the second-level translation (EPT/NPT) finds no memslot there which is what makes
#       KVM to return KVM_EXIT_MMIO

.code64
.text
.global _start

.set    UART_DATA,          0x3f8
.set    UART_LSR,           0x3fd
.set    CTL_BASE,           0xd0000000
.set    CTL_ID,             0x00
.set    CTL_COUNT,          0x04
.set    CTL_POWEROFF,       0x08
.set    ALIAS_VA,           0x200000            # maps to guest-physical 0x20000
.set    DATA_VA,            0x20000             # identity mapped: virtual address 0x20000 == physical 0x20000
.set    MSR_EFER,           0xC0000080

_start:
    mov $0x90000, %esp                          # move stack to RAM (zero extends into rsp)

    lea s_banner(%rip), %rsi
    call puts


    #--- 1. control registers -> proving that this is realing long mode with paging ---
    lea s_cr3(%rip), %rsi
    call puts                                   # should ouput CR3 value?
    mov %cr3, %rdi                              # no VM exit: with EPT, the CPU lets the guest read CR3
    call print_hex64
    call newline

    lea s_cr0(%rip), %rsi
    call puts
    mov %cr0, %rdi
    call print_hex64
    call newline

    lea s_efer(%rip), %rsi
    call puts
    mov $MSR_EFER, %ecx
    rdmsr                                       # edx:eax = EFER. (KVM usually handles this inside the kernel)
    shl $32, %rdx
    or  %rdx, %rax
    mov %rax, %rdi
    call print_hex64
    call newline

    #--- 2. one physical page, two virtual addresses ---
    lea s_alias_w(%rip), %rsi
    call puts
    mov $ALIAS_VA, %ebx
    mov $DATA_VA, %r12d
    movl $0xfeedface, (%rbx)                    # store 0xfeedface to the value pointed to by the address in rbx.
    mov (%r12), %edi                            # .. and load through the identiy address i.e. mov the value in R12 to EDI.
    call print_hex64
    call newline

    lea s_alias_r(%rip), %rsi
    call puts
    movl $0x0badf00d, (%r12)                    # store through the identity address ...
    mov (%rbx), %edi                            # ...and load through the alias
    call print_hex64
    call newline


    #--- 3. MMIO via a virtual address ---
    mov $CTL_BASE, %ebx                         # NOTE %ebx, not %rbx: `mov $imm32, %rbx` would SIGN-extend 0xd0000000 into 0xffffffffd0000000 and thereby loading a different address 
    lea s_id(%rip), %rsi
    call puts
    mov CTL_ID(%rbx), %edi
    call print_hex64
    call newline

    lea s_count(%rip), %rsi
    call puts
    mov CTL_COUNT(%rbx), %edi
    call print_hex64
    call newline

    #--- 4. power off with exit code 3 ---
    lea s_bye(%rip), %rsi
    call puts
    movl $3, CTL_POWEROFF(%rbx)
1:  hlt                                         # not reached
    jmp 1b                                      # if reached by any chance will be an infinite loop.

#putc function: al - character. Poll the line-status register (i.e. LSR UART register), then send 2 VM exits per character. The catcher of the exit should display it.
putc:
    push %rdx
    push %rax
    mov $UART_LSR, %dx
1:  in %dx, %al
    test $0x20, %al                             # loop will run 20 times?
    jz 1b
    pop %rax
    mov $UART_DATA, %dx
    out %al, %dx
    pop %rdx
    ret

#puts: rsi -> NUL-terminated sting
puts:
    push %rax
1:  lodsb                                       # al = [rsi], rsi++
    test %al, %al
    jz 2f
    call putc
    jmp 1b
2:  pop %rax
    ret

newline:
    push %rax
    mov $0x0a, %al
    call putc
    pop %rax
    ret

#print_hex64: rdi=value, prints 16 lowercase hex digits
print_hex64:
    push %rax
    push %rcx
    push %rdx
    push %r8
    lea hexdigits(%rip), %r8
    mov $16, %ecx
1:  rol $4, %rdi
    mov %rdi, %rdx
    and $0xf, %edx
    mov (%r8, %rdx, 1), %al
    call putc
    loop 1b
    pop %r8
    pop %rdx
    pop %rcx
    pop %rax
    ret

hexdigits:          .ascii          "0123456789abcdef"

s_banner:           .asciz          "tinyvm m3: guest is up (64-bit long mode, paging on)\n"
s_cr3:              .asciz          "CR3 (page table root)              :0x"
s_cr0:              .asciz          "CR0 (PE/PG bits)                   :0x"
s_efer:             .asciz          "EFER (LME=bit8, LMA=bit10)         :0x"
s_alias_w:          .asciz          "write VA 0x200000, read VA 0x20000 :0x"
s_alias_r:          .asciz          "write VA 0x20000, read VA 0x200000 :0x"
s_id:               .asciz          "MMIO via 0xd0000000, identity      :0x"
s_count:            .asciz          "MMIO access counter                :0x"
s_bye:              .asciz          "powering off with code 3\n"
