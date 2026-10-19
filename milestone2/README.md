# tinyvm milestone 2: devices behind a bus

```
make test     # device model tests, plain C++, no KVM needed
make          # builds ./tinyvm and guest.bin (needs g++, as, ld)
./tinyvm      # needs Linux + /dev/kvm
```

## Files

| File | Role |
|---|---|
| `device.h` | `Device` interface and `Bus` (address to device routing, overlap checks, open-bus reads) |
| `devices.h` | `Uart16550` (port I/O) and `ControlDevice` (MMIO) |
| `guest.S` | 32-bit guest; assembled to a flat `guest.bin` loaded at guest-physical 0x1000 |
| `main.cpp` | KVM setup, protected-mode vCPU, run loop turning `KVM_EXIT_IO` / `KVM_EXIT_MMIO` into bus accesses |
| `test_bus.cpp` | Unit tests for everything except the KVM glue |

## Expected output (derived from the guest source; not captured from a real KVM run)

```
tinyvm m2: guest is up (32-bit protected mode)
control device ID      : 0x324d5654
access counter x3      : 0x00000002 00000003 00000004
scratch reg round trip : 0x12345678
plain RAM round trip   : 0xcafebabe
unmapped read (0xdead0000): 0xffffffff
powering off with code 7

--- vm stopped ---
guest powered off with exit code 7
VM exits: 554 total
  port I/O  in/out : 273 / 273
  MMIO read/write  : 6 / 2
  ...
```

The exit counts follow from the guest: every printed character costs one `in` (poll the status register) and one `out`, and there are 273 characters. The MMIO side is 6 reads (ID, 3 counter reads, scratch read, the unmapped read) and 2 writes (scratch, poweroff). The control device sees 7 of those 8, since the unmapped read never reaches it. The plain-RAM round trip causes no exit at all.

## Exercises

1. Add a second `Uart16550` at 0x2f8 (COM2) with its own output stream. How many lines change? That is the point of the bus.
2. Make `Bus::attach` reject a device that overlaps a KVM memslot. Where would you keep the list of RAM regions?
3. Add a fourth counter read to the guest and update the expected counter values. Then change `putc` so it skips the status poll, and watch the exit count drop by about half.
4. Give `ControlDevice` a register that returns the host's monotonic clock in nanoseconds. What would break if the guest used it for timing?
5. Read `KVM_IRQFD` / `KVM_IOEVENTFD` in the kernel docs: how can the kernel complete a device access without waking your thread at all? (Hint: this is the idea behind vhost.)