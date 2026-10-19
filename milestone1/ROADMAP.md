# tinyvm roadmap

A learning project: each milestone is small enough to finish in a sitting and maps to a topic on your list.
Milestone 1 is `main.cpp`. The rest are what to build next.

## Milestones

| # | Milestone | Topics | What you build | What you should be able to explain afterwards |
|---|-----------|--------|----------------|-----------------------------------------------|
| 1 | **KVM hello** (done) | 1, 2, 3, 4 | Open `/dev/kvm`, create VM, map RAM, run a 16-bit guest, handle `KVM_EXIT_IO` and `KVM_EXIT_HLT` | What a VM exit is; why `out` and `hlt` trap; who does what: your process vs. kernel vs. CPU |
| 2 | **Device bus** | 5 | A `Device` interface with `pio_read/write` and `mmio_read/write`, a bus that dispatches by address, a 16550-style UART, and an MMIO region with no RAM behind it (`KVM_EXIT_MMIO`) | Port I/O vs. MMIO; why every emulated register access costs a VM exit; why virtio exists |
| 3 | **Long mode + page tables** | 3, 4 | Build identity-mapped page tables by hand, switch the guest to 64-bit mode, run a tiny 64-bit payload | Guest-virtual to guest-physical (guest page tables) to host-physical (EPT/NPT); what the hypervisor does not see |
| 4 | **TAP-backed NIC** | 6, 8 | Open `/dev/net/tun` with `IFF_TAP`; expose an MMIO NIC with TX/RX buffers; guest sends an Ethernet frame, you write it to the TAP fd; poll the fd for RX | What a TAP device is; frames vs. packets; why this is the same job QEMU's `-netdev tap` does |
| 5 | **Virtio-net concepts** | 5, 6 | Replace the toy NIC with a minimal virtqueue (descriptor table, avail ring, used ring); read the virtio spec section 2 | Why virtio avoids per-register exits; what vhost-net moves into the kernel |
| 6 | **Networking labs (no C++)** | 7, 8 | With `ip`, `bridge`, `nft`, `dnsmasq`: build a bridge, attach the TAP, then do the same behind NAT | NAT vs. bridge vs. virtual switch, in terms of who owns the IP, who answers ARP/DHCP, who is reachable from the LAN |
| 7 | **Backend abstraction** | 9, 10, 11 | `class Hypervisor { create_vm; map; create_vcpu; run }` with `KvmBackend` implemented and `WhpxBackend` / `HvfBackend` as stubs, using the table below | Why Multipass has a per-platform backend layer; what stays common (VM lifecycle, images, cloud-init) vs. what differs (accelerator, networking) |

Order is flexible, but 1, 2, 4 and 6 give you the most interview mileage for the least code.

## The same four verbs on three platforms

Every hypervisor API reduces to: create a VM, map guest memory, create a vCPU, run it and handle exits.

| Concept | Linux KVM (`ioctl`) | Windows WHPX (`WinHvPlatform`) | macOS Hypervisor.framework |
|---|---|---|---|
| Create VM | `KVM_CREATE_VM` | `WHvCreatePartition` + `WHvSetPartitionProperty` + `WHvSetupPartition` | `hv_vm_create` |
| Map guest RAM | `KVM_SET_USER_MEMORY_REGION` | `WHvMapGpaRange` | `hv_vm_map` |
| Create vCPU | `KVM_CREATE_VCPU` | `WHvCreateVirtualProcessor` | `hv_vcpu_create` |
| Run | `KVM_RUN` | `WHvRunVirtualProcessor` | `hv_vcpu_run` |
| Port I/O exit | `KVM_EXIT_IO` | `WHvRunVpExitReasonX64IoPortAccess` | none on Apple silicon (ARM64 has no port I/O) |
| MMIO exit | `KVM_EXIT_MMIO` | `WHvRunVpExitReasonMemoryAccess` | data abort exit, decoded from the exception syndrome |
| Who provides in-kernel devices (interrupt controller, timers) | KVM (in-kernel irqchip, optional) | Hyper-V platform (APIC emulation available) | You, or the framework's GIC support on newer macOS |

Things to notice:

- KVM is a kernel module you drive through ioctls. WHPX and Hypervisor.framework are user-mode APIs in front of a different hypervisor (Hyper-V on Windows, Apple's on macOS). On Windows, WHPX is what lets QEMU use hardware acceleration while Hyper-V owns VMX.
- Apple silicon is ARM64, so the x86 guest code in `main.cpp` does not run there. On a Mac, an equivalent hello-world would run an ARM64 guest and handle MMIO exits (a UART at a fixed address) instead of port I/O.
- None of these give you a machine. You still supply firmware, devices and a boot protocol, which is the work QEMU does.

## QEMU vs. KVM in one paragraph

KVM turns the Linux kernel into a hypervisor: it uses VMX (Intel) or SVM (AMD) so guest code runs directly on the CPU, and it handles the exits it can handle cheaply. KVM has no concept of disks, NICs, firmware or a machine layout. QEMU is the user-space VMM: it creates the VM through KVM, allocates guest RAM, emulates the devices and handles the exits that KVM passes up (your `run_loop`). Without KVM, QEMU can still run guests by emulating the CPU in software (TCG), only much more slowly. `main.cpp` is a miniature QEMU.

## Where Multipass sits (verify details in the repo and docs before the interview)

- Multipass is a CLI and daemon (`multipassd`) that launches Ubuntu VMs quickly. It does not implement virtualization itself.
- It defines an abstraction over hypervisors, built around a VM factory per backend (look for `VirtualMachineFactory` in the `canonical/multipass` repo). Backends include QEMU, Hyper-V, VirtualBox, libvirt and LXD, depending on platform and version.
- Typical defaults: QEMU on Linux and macOS, Hyper-V on Windows. Check the current docs for exact defaults and for which backends are supported where.
- Multipass drives backends, images (Ubuntu cloud images), provisioning (cloud-init) and networking for you. Milestone 7 is the toy version of the same layering.
- Networking: on Linux with QEMU, expect a host bridge created by the daemon, one TAP per instance, and NAT plus DHCP/DNS for the bridge's subnet. Milestones 4 and 6 reproduce that by hand. Look at the actual bridge name, `ip addr` and your firewall rules on a machine running Multipass to confirm.

## Interview-style questions to practice answering out loud

1. What happens, step by step, between a guest executing `out dx, al` and your C++ handler running?
2. Why is KVM useless on its own, and why is QEMU slow without it?
3. What do EPT and NPT add compared to shadow page tables?
4. Why is virtio faster than emulating an e1000 NIC?
5. A VM can reach the internet but nothing on the LAN can reach the VM. Is it NAT or bridged, and how do you tell?
6. What would you need to change in a backend abstraction to add a new hypervisor?
7. Why does Multipass need a different backend on Windows, and what does WHPX have to do with it?

## Running milestone 1

```
make
./tinyvm
```

Requires Linux with `/dev/kvm` and permission to open it (usually the `kvm` group). Under WSL2 or a cloud VM you need nested virtualization enabled. Expected output: the guest message, then a short report of VM exit counts and the GPA-to-HVA line.
