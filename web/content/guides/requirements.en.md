What each LordOS image needs. If a figure here and another page disagree, this page is the one we keep up to date.

## At a glance

| | PC (ISO) | Virtual machine | Raspberry Pi 5 | Debian 13 package |
|---|---|---|---|---|
| File | `lordos-live-<version>-amd64.iso` | `lordos-<version>-amd64.ova`, or an ISO | `lordos-<version>-rpi5-arm64.img.xz` | `install.sh` |
| Processor | 64-bit Intel or AMD (amd64) | at least 4 CPUs | Raspberry Pi 5 | amd64 or arm64 |
| Memory | 4 GB minimum, 8 GB recommended | 4 GB minimum, 16 GB ideal | 4 GB works, 8 or 16 GB recommended | as for a PC |
| Storage | 8 GB disk minimum, 32 GB recommended | 32 GB disk (the OVA's grows as used) | SD card of 16 GB (8 GB minimum) or NVMe | 2 GB free in `/var` |
| To install from | a USB stick of 2 GB or more | the OVA file | an SD card or NVMe drive | a Debian 13 system |

## PC

- **Processor:** any 64-bit Intel or AMD processor. There is no 32-bit version.
- **Memory:** 4 GB minimum; 8 GB recommended, 16 GB with many apps open.
- **Storage:** installing needs a disk of at least 8 GB (the installer's limit is 8 GiB); 32 GB or more is recommended. Installing next to another system needs a GPT disk with an EFI partition, the computer started in UEFI mode, and at least 8 GB of free space. That option has never been run to the end yet, so back up first.
- **Firmware:** UEFI or legacy BIOS. Turn Secure Boot off.
- **Graphics:** any. With a supported graphics card (through Mesa: AMD, Intel and others) the desktop is composed on the GPU; without one, the CPU path always works.
- **To start it:** a USB stick of 2 GB or more; the ISO is about 1.1 GB. The live system can also be copied to memory at start (an entry in the boot menu); give the computer 4 GB or more for that.

## arm64 ISO

`lordos-live-<version>-arm64.iso` is for arm64 machines and virtual machines that start with UEFI. So far it has been run only on emulated QEMU, not on real hardware. The memory and storage figures are the PC's.

## Virtual machine

- **Processors and memory:** at least 4 CPUs and 4 GB of memory. 2 CPUs are too few for good performance. With many apps open, 16 GB is the ideal.
- **Hypervisor:** the OVA opens in VirtualBox 7 and VMware; the ISO works in those and in others, such as Proxmox. For the ISO, UEFI is recommended and the guest type is Debian 64-bit.
- **The computer running it:** a 64-bit PC with hardware virtualization on in its firmware, and the machine's memory free on top of its own use.
- **Storage:** the OVA's disk is 32 GB and grows as it is used.

Step by step: [Virtual machine (OVA)](/virtual-machine/).

## Raspberry Pi 5

- **Board:** Raspberry Pi 5 with 4, 8 or 16 GB. 4 GB works; 8 or 16 GB is recommended. Other Raspberry Pi models are not supported.
- **Storage:** an SD card of 16 GB or more (8 GB minimum), or an NVMe drive on the M.2 HAT+, which is faster.
- **Power and cooling:** the official 27 W power supply (5 V, 5 A) and active cooling are recommended.

The [LordOS Imager](/imager/) writes the image and prepares the first boot.

## On an existing Debian 13

`curl -fsSL https://lordos.org/install.sh | sh` adds LordOS to Debian 13 (trixie), amd64 or arm64, started with systemd. It needs root or `sudo` and 2 GB free in `/var`. Other distributions based on Debian may work, with a warning.

## Not supported

- 32-bit processors.
- Starting with Secure Boot on.
- Raspberry Pi models other than the Pi 5.
