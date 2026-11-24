/* machine.h — a machine's description, machine.json (docs/VIRTUALIZATION.md 3.5, 3.6, 3.7 and "Phase 1
 * implementation"). One folder per machine:
 *
 *   <dir>/machine.json        this description, schema-versioned; never holds a password
 *   <dir>/disks/system.qcow2  the system disk
 *   <dir>/disks/nvram.qcow2   UEFI variables, qcow2 so that internal snapshots work (6.2); absent with "bios"
 *   <dir>/log/                lord-vm.log (this run) and lord-vm.1.log, lord-vm.2.log (the two before), serial.log
 *   <dir>/lock                held (flock) by the running lord-vm
 *   <dir>/run.json            the last state lord-vm wrote: state, why, pids; read by status and recovery
 *
 * QEMU may write only inside disks/: it can't change machine.json, the logs or run.json.
 *
 * Parsing is strict: unknown keys, wrong types and out-of-range values are refused, and a file with a newer
 * schema is never rewritten (VM_E_NEWER) and never started. Limits that depend on the computer (vCPUs, memory,
 * KVM) are checked separately, against a VmHost, so the same file can be read on another computer. */
#ifndef LORD_VM_MACHINE_H
#define LORD_VM_MACHINE_H

#include <stddef.h>
#include <stdint.h>

/* 1: Phase 1. 2 (Beta): base, agent, clipboard. 3 (Import, 6.22): devices, nic_model, extra_nics, cpu_sockets/cores/threads,
 * smbios, disk_serial, data_disks, rtc. 4 (0.8.8.7): shares. 5: secure_boot, tpm, hyperv, drivers, the windows devices,
 * graphics. 6: the bridged and host-only networks, bridge, ports (6.5); sound, volume, microphone (6.9); boot_order, boot_menu. 7: transfer (6.7.1). 8: sound_out, sound_in (6.9). 9: clipboard_ask (6.8). 10: pointer (7.1.2). 11: gpu (docs/MACHINES-GPU.md, Mode A). 12: capture_fullscreen (6.25). 13: perf_profile (7.1.4), cpu_pin (3.7.1). 14: group, role, the "link" network, uplink, link_mac, profile_lock, timers (machine groups, 19.4). 15: guest_links, guest_notify (6.12). 16: disposable, encrypted, backup, disk_format (storage: 6.2, 6.4, 8). 17: automation (6.15, `machines exec` and `cp`). 18: cpu_model (5.5). 19: heads (5.4, All Displays). 20: reserved (guest_files). 21: usb, usb_auto (6.6). An older file is read as
 * is and written as the current schema, with a copy of the old
 * one kept beside it (machine.json.schema1); a newer one is never rewritten. */
#define VM_SCHEMA 21
/* the most displays (virtio-gpu outputs) a machine may have: "heads" (schema 19) */
#define VM_HEADS_MAX 4
#define VM_USB_AUTO     8               /* schema 21: auto-connect rules (usb_auto) */
#define VM_USB_RULE_MAX 73              /* "vvvv:pppp:" and a serial of up to 63 */

enum { VM_ARCH_ARM64 = 0, VM_ARCH_AMD64 };
enum { VM_ACCEL_KVM = 0, VM_ACCEL_TCG };
enum { VM_FW_UEFI = 0, VM_FW_BIOS };
enum { VM_DISPLAY_WINDOW = 0, VM_DISPLAY_NONE };
/* the machine's network (6.5): shared (passt, out only, plus the forwarded ports), isolated (no card's backend), bridged
 * (a macvtap on one of this computer's wired cards: the guest is another device on that network, by its DHCP), host-only
 * (passt whose sockets are bound to 127.0.0.1: the guest reaches this computer and nothing else) */
enum { VM_NET_SHARED = 0, VM_NET_ISOLATED, VM_NET_BRIDGED, VM_NET_HOSTONLY, VM_NET_LINK };
extern const char *const VM_NET_NAMES[];   /* "shared", "isolated", "bridged", "hostonly", "link" */
/* schema 14, machine groups (docs/VIRTUALIZATION.md 19): "link" (a group's workstation only) is one card wired to nothing
 * but the group's internal link (VM_LINK): no passt, no host route. A member's role in its group, its profile lock and the
 * clock devices its kind asks for */
enum { VM_ROLE_NONE = 0, VM_ROLE_GATEWAY, VM_ROLE_WORKSTATION, VM_ROLE_COUNT };
extern const char *const VM_ROLE_NAMES[];  /* "", "gateway", "workstation" */
enum { VM_LOCK_NONE = 0, VM_LOCK_ANONYMITY };
extern const char *const VM_LOCK_NAMES[];  /* "", "anonymity" */
enum { VM_TIMERS_DEFAULT = 0, VM_TIMERS_WHONIX_GATEWAY, VM_TIMERS_WHONIX_WORKSTATION };
extern const char *const VM_TIMERS_NAMES[];   /* "", "whonix-gateway", "whonix-workstation" */
/* schema 16, a disposable session (docs/VIRTUALIZATION.md 6.2): every writable disk, the UEFI variables and the TPM state
 * get a temporary layer at each start, thrown away when QEMU ends: in memory (a memfd, capped at the machine's memory and
 * counted in its limit) or on this computer's disk (an unlinked file in disks/, which nothing names after QEMU ends) */
enum { VM_DISP_OFF = 0, VM_DISP_MEMORY, VM_DISP_DISK };
extern const char *const VM_DISP_NAMES[];  /* "", "memory", "disk" */
/* the internal link's card: the workstation's only card is in slot 0x2 (its eth0), the gateway's at 0x18 (its eth1, after
 * passt's card in 0x2); each on a QEMU hub of its own so that its backend can come and go (19.3) */
#define VM_LINK_SLOT_GATEWAY 0x18
/* forwarded ports (6.5): a port of this computer that passt opens and forwards to one of the guest's */
enum { VM_PROTO_TCP = 0, VM_PROTO_UDP };
enum { VM_OPEN_LOCAL = 0, VM_OPEN_LAN };    /* "this-computer" binds 127.0.0.1, "lan" 0.0.0.0 */
#define VM_PORTS        32
#define VM_PORT_HOST_MIN 1024           /* below it a port is root's: never offered */
typedef struct { int proto, host, guest, open; } VmPort;
/* the guest's addresses in host-only mode: passt hands them by DHCP; this computer is the gateway */
#define VM_HOSTONLY_ADDR "10.0.2.15"
#define VM_HOSTONLY_GW   "10.0.2.2"
#define VM_IFNAME_MAX    15             /* IFNAMSIZ - 1 */
/* the shared clipboard's direction (6.8): off unless chosen */
enum { VM_CLIP_OFF = 0, VM_CLIP_TO_GUEST, VM_CLIP_FROM_GUEST, VM_CLIP_BOTH };
/* the disks' and the pointer's devices (6.22): virtio (fast, needs the guest's drivers) or compat, the ones every system has
 * drivers for: SATA (AHCI) disks and CD/DVD, a USB tablet. The network card has its own model (nic_model). */
enum { VM_DEV_VIRTIO = 0, VM_DEV_COMPAT, VM_DEV_WINDOWS };
/* VM_DEV_WINDOWS (schema 5): virtio disks and network with the CD/DVD drives on SATA and a USB tablet, so Windows Setup
 * reads its DVD and the virtio-win drivers' with the drivers it has, and loads virtio's from them */
extern const char *const VM_DEV_NAMES[];   /* "virtio", "compat", "windows" */
/* network card models: virtio-net-pci and the emulated cards an imported machine may have had */
enum { VM_NIC_VIRTIO = 0, VM_NIC_E1000E, VM_NIC_E1000, VM_NIC_E1000_82545EM, VM_NIC_E1000_82544GC, VM_NIC_VMXNET3, VM_NIC_RTL8139, VM_NIC_PCNET, VM_NIC_COUNT };
extern const char *const VM_NIC_MODELS[VM_NIC_COUNT + 1];   /* "virtio", "e1000e", ... (machine.json's names) */
enum { VM_RTC_UTC = 0, VM_RTC_LOCAL };
/* vCPU pinning (schema 13): this computer's default, off, or auto (lord-vm picks the cores, vmpin.c) */
enum { VM_PIN_DEFAULT = 0, VM_PIN_OFF, VM_PIN_AUTO };
enum { VM_CPU_HOST = 0, VM_CPU_COMPAT };
extern const char *const VM_CPU_NAMES[];  /* "host", "compatible" */
extern const char *const VM_PIN_NAMES[];   /* "", "off", "auto" */
/* the guest's graphics (schema 5): 2D, a virtio GPU whose picture QEMU draws in shared memory; 3D, virgl (virtio-vga-gl),
 * which opens the host GPU's render node from QEMU's sandbox: Linux guests only (no device set "windows") */
enum { VM_GFX_2D = 0, VM_GFX_3D };
/* a real GPU for the machine (schema 11, docs/MACHINES-GPU.md Mode A): none; an Intel GVT-g vGPU of the largest type the
 * iGPU lists ("intel-whole", close to the whole iGPU for this machine), or of the smallest that shows the machine's
 * resolution ("intel-shared", so several machines can have one). The vGPU is then the machine's only display */
enum { VM_VGPU_NONE = 0, VM_VGPU_WHOLE, VM_VGPU_SHARED };
extern const char *const VM_VGPU_NAMES[];  /* "", "intel-whole", "intel-shared" */
/* the boot order (schema 6): the devices the firmware tries, each once; none listed is "auto" */
enum { VM_BOOT_DISK = 0, VM_BOOT_CD, VM_BOOT_NET, VM_BOOT_DEVS };
extern const char *const VM_BOOT_NAMES[];  /* "disk", "cd", "network" */
#define VM_EXTRA_NICS  7            /* cards beyond the first, carried from an import: present with their MAC, not connected */
#define VM_DATA_DISKS  4            /* disks/data-1.qcow2 ... data-4.qcow2 */
#define VM_SMBIOS_MAX  64           /* bytes of printable ASCII per SMBIOS string */
#define VM_SERIAL_MAX  20           /* a disk's serial: ATA's 20 characters */
#define VM_SHARES      4            /* shared folders (6.7), each a vhost-user-fs device on PCI slots 0xb to 0xe */
#define VM_SOUND_DEV_MAX 96         /* sound_out, sound_in: a node.name; two fit one VM_SOUND message (6.9) */
#define VM_XFER_TAG    "LordOS"     /* the transfer folder's mount tag (6.7.1); its device is on slot 0xf */
#define VM_XFER_DIR    "Transfer"   /* <library>/Transfer/<machine folder's name without .lordvm>: the transfer folders */
#define VM_TAG_MAX     32           /* a share's mount tag: letters, digits, '-', '_' (virtio-fs allows 36 bytes) */

/* Limits that hold on every computer */
#define VM_NAME_MAX       64            /* bytes of UTF-8 */
#define VM_VCPUS_MAX      64
#define VM_MEM_MIN_MB     256
#define VM_MEM_MAX_MB     (1024 * 1024)
#define VM_DISK_MIN_GB    1
#define VM_DISK_MAX_GB    16384
#define VM_PATH_MAX       1024
/* what the desktop keeps for itself (3.7): 2 GB, or 1 GB on a computer with 4 GB or less */
#define VM_FLOOR_MB       2048
#define VM_FLOOR_SMALL_MB 1024
/* QEMU's own memory over the guest's, for the machine's memory limit: measured 2026-11-04 at 1–2 GB guests,
 * QEMU's RSS was 409–582 MB over what the guest had touched at login (section 13.2); TCG with tb-size=256 */
#define VM_OVERHEAD_KVM_MB 384
#define VM_OVERHEAD_TCG_MB 640

typedef struct {
    int schema;
    char id[37];                        /* lowercase UUID; also QEMU's -uuid */
    char name[VM_NAME_MAX + 1];
    int arch, accel, vcpus;
    int memory_mb;
    int disk_gb;                        /* system disk's virtual size */
    int firmware;
    char machine_type[32];              /* pinned at creation: "virt-10.0", "pc-q35-10.0" */
    char firmware_code[VM_PATH_MAX];    /* UEFI code file read at start ("" with bios) */
    char firmware_sha256[65];           /* its identity when the NVRAM was made ("" with bios) */
    int display;
    int network;
    char mac[18];                       /* 52:54:00:xx:xx:xx */
    char iso[VM_PATH_MAX];              /* attached installer image, read-only; "" none */
    char base[VM_PATH_MAX];             /* a linked clone's frozen shared disk (6.3): <library>/.shared/<x>.qcow2, read
                                         * only, the system disk's backing; "" none (schema 2) */
    int agent;                          /* the guest agent channels (6.17): qemu-guest-agent and org.lordos.agent; off
                                         * unless turned on (schema 2) */
    int clipboard;                      /* VM_CLIP_*, through org.lordos.agent (schema 2) */
    int clipboard_ask;                  /* "ask before each paste" into and copy out of the machine (schema 9; false: trusted) */
    /* schema 3: what an imported machine keeps of its source's hardware (6.22) */
    int devices;                        /* VM_DEV_* */
    int nic_model;                      /* VM_NIC_* */
    char extra_macs[VM_EXTRA_NICS][18];       /* extra_nics: each card's MAC and model */
    int extra_models[VM_EXTRA_NICS];          /* VM_NIC_* */
    int n_extra_macs;
    int cpu_sockets, cpu_cores, cpu_threads;  /* 0: QEMU's default topology; else their product is vcpus */
    char sm_manufacturer[VM_SMBIOS_MAX + 1], sm_product[VM_SMBIOS_MAX + 1], sm_version[VM_SMBIOS_MAX + 1],
         sm_serial[VM_SMBIOS_MAX + 1], sm_sku[VM_SMBIOS_MAX + 1], sm_family[VM_SMBIOS_MAX + 1];
    char disk_serial[VM_SERIAL_MAX + 1];
    int n_data;                         /* data disks: disks/data-<i+1>.qcow2 */
    int data_gb[VM_DATA_DISKS];
    char data_serial[VM_DATA_DISKS][VM_SERIAL_MAX + 1];
    int rtc;                            /* VM_RTC_* */
    /* schema 4: folders of this computer the guest mounts (6.7), each served by its own virtiofsd; read-only unless
     * share_rw. The path is checked again where it's used (vm_share_allowed): this file only keeps it */
    int n_shares;
    char share_tag[VM_SHARES][VM_TAG_MAX + 1];
    char share_path[VM_SHARES][VM_PATH_MAX];
    int share_rw[VM_SHARES];
    /* schema 5: what Windows 11 asks of a machine (docs/VIRTUALIZATION.md 4.3), amd64 only for now */
    int secure_boot;                    /* UEFI with secure boot and Microsoft's keys enrolled (OVMF .secboot + .ms) */
    int tpm;                            /* a TPM 2.0, served by swtpm; its state in disks/tpm */
    int hyperv;                         /* Hyper-V enlightenments, for Windows guests */
    char drivers[VM_PATH_MAX];          /* a second CD/DVD with the guest's drivers (virtio-win), read-only; "" none */
    int graphics;                       /* VM_GFX_*; "graphics": "3d" is written only when chosen (schema 5) */
    int res_w, res_h;                   /* "resolution": "WxH", the display the guest starts with (its EDID's preferred
                                         * mode, which every driver reads at boot); 0 x 0: it follows the window (schema 6) */
    /* schema 6: the network's details (6.5) */
    char bridge[VM_IFNAME_MAX + 1];     /* the wired card a bridged machine joins ("" none); kept when the mode changes */
    int n_ports;
    VmPort ports[VM_PORTS];             /* used in shared and host-only modes; kept in the others */
    /* schema 6: sound and microphone (6.9). Host-side choices, changed while the machine runs too: the guest's sound
     * card stays the same either way. A file without them reads as sound on at full volume, microphone off */
    int sound;                          /* 1: the guest's sound plays on this computer's speakers; 0 muted */
    int volume;                         /* 0..100, the machine's own stream volume */
    int microphone;                     /* 1: the guest may hear this computer's microphone, once LordOS's microphone
                                         * permission for Machines allows it too (checked by lord-server); 0 silence */
    /* schema 6: where the machine starts from. n_boot 0 is "auto" (no "boot_order" in the file): the system disk, then
     * the CD/DVD drive; an empty disk doesn't boot, so a new machine starts from its ISO until a system is installed.
     * Else the firmware tries boot_order[0..n_boot-1] in that order (QEMU's bootindex); what isn't listed gets no boot
     * index (SeaBIOS and OVMF may still try it after the listed ones). boot_menu: the firmware waits a few seconds at each start for Esc and its boot menu */
    int n_boot;
    int boot_order[VM_BOOT_DEVS];       /* VM_BOOT_*, each at most once */
    int boot_menu;
    /* schema 7: the transfer folder (6.7.1), <library>/Transfer/<name>, shared writable under the tag VM_XFER_TAG. On for
     * new machines; a file without it reads as off, so a machine made before keeps its hardware (and its saved states) */
    int transfer;
    /* schema 8: the output the guest's sound plays on and the input its microphone hears (6.9), each a PipeWire node.name
     * (vm_sound_dev_valid); "" (or no key) the system's default, which follows what Settings → Sound picks. A device that
     * isn't there when used falls back to the default (lord-server), it isn't an error */
    char sound_out[VM_SOUND_DEV_MAX + 1], sound_in[VM_SOUND_DEV_MAX + 1];
    /* schema 10: who draws a Windows guest's pointer (docs/VIRTUALIZATION.md 7.1.2): "pointer": "guest" its display driver,
     * into the picture (viogpudo's HWCursor 0, upstream's default); missing or "lordos": LordOS, from the shape the guest
     * hands over (HWCursor 1, set by lord-vm through the guest tools' agent when they say "pointer") */
    int pointer_guest;
    /* schema 11: VM_VGPU_* ("gpu": "intel-whole" | "intel-shared", written only then); amd64 UEFI machines with 2D graphics */
    int vgpu;
    /* schema 12: "Capture mouse" (docs/VIRTUALIZATION.md 6.25): "capture_fullscreen": true, the window captures the mouse
     * when it goes full screen (written only then; missing: no) */
    int capture_fullscreen;
    /* schema 13: the guest's performance profile (docs/VIRTUALIZATION.md 7.1.4): "perf_profile": true, lord-vm asks the
     * guest tools that say "perf" to apply it (AGK_PERF) while the machine runs (written only then; missing: no) */
    int perf_profile;
    /* schema 13: vCPU pinning (docs/VIRTUALIZATION.md 3.7.1): "cpu_pin": "off" | "auto", written only when chosen; missing
     * is this computer's default for the machine (vmpin_default_on) */
    int cpu_pin;                        /* VM_PIN_* */
    /* schema 15: guest → host links and notifications (docs/VIRTUALIZATION.md 6.12), off unless chosen (written only
     * when on): "guest_links": true, a link the guest's tools send is offered in a bar the person must click;
     * "guest_notify": true, the guest's notifications are shown as from the machine, rate-limited */
    int guest_links, guest_notify;
    /* schema 19: the guest's displays (docs/VIRTUALIZATION.md 5.4, All Displays): the virtio-gpu's outputs, 1 (missing) to
     * VM_HEADS_MAX; written only when more than one. Hardware the guest sees: changed only while the machine is off */
    int heads;
    /* schema 18: the processor the guest sees (docs/VIRTUALIZATION.md 5.5, Configure → CPU and memory), written only when
     * chosen: "cpu_model": "compatible", a fixed x86-64-v2 model (amd64 with KVM only: the guest moves between computers and
     * keeps working after this one's processor changes); missing: "host", this computer's own processor */
    int cpu_model;                      /* VM_CPU_HOST, VM_CPU_COMPAT */
    /* schema 14: a member of a machine group (docs/VIRTUALIZATION.md 19.4), written only for members. group: the group's
     * id (group.json "id"); role: VM_ROLE_*; uplink_tor: the gateway's passt has no DNS forwarding and no forwarded ports
     * ("uplink": "tor", with network "shared"); link_mac: the gateway's second card, on the internal link; profile_lock:
     * VM_LOCK_ANONYMITY refuses every host-to-guest channel (19.5: no agent, clipboard, transfer, shares, ports, 3D, vGPU,
     * microphone) in vm_machine_check, so --set and Configure can't turn one on; timers: VM_TIMERS_* (19.5, "Time") */
    char group[37];
    int role;
    int uplink_tor;
    char link_mac[18];
    int profile_lock;
    int timers;
    /* schema 16: storage (docs/VIRTUALIZATION.md 6.2, 6.4, 8), each written only when it differs from a new machine.
     * disposable: VM_DISP_* ("disposable": "memory" | "disk"); never on a group's member (its group.json has the switch).
     * encrypted: "encrypted": true, every disk and the UEFI variables are qcow2 with LUKS and the TPM state is swtpm's
     * encrypted state, all under one key that lives in the session keyring (owner "machines", service "machines:<id>"),
     * handed to lord-vm on a descriptor and to QEMU and qemu-img through a pipe (never a file, an argument or the
     * environment). raw: "disk_format": "raw", disks/system.raw and data-N.raw, plain files (fastest, no snapshots, no
     * encryption, no linked clone). no_backup: "backup": false, Backups leaves the folder out */
    int disposable;
    int encrypted;
    int raw;
    int no_backup;
    /* schema 17: the person's consent to automation (docs/VIRTUALIZATION.md 6.15, "lordvm and the agent"): "automation": true
     * lets `machines exec`, `machines cp` and the MCP server's run_command run programs in the guest, as its signed-in
     * person, through lord-vm (VM_EXEC); written only then, missing: no. Never with VM_LOCK_ANONYMITY */
    int automation;
    /* schema 21: host USB devices that go into the machine by themselves while it runs (docs/VIRTUALIZATION.md 6.6):
     * "usb_auto": ["046d:0843", "0781:5581:4C530001"], each vendor:product (four lowercase hex digits each) and maybe the
     * device's serial (1 to 63 of [A-Za-z0-9._-]); at most VM_USB_AUTO, no two the same (vm_usb_rule_valid). Lord-server
     * connects a device that matches while the machine runs (lord.vm.usbAttach's checks, the admin helper's vm-usb-allow);
     * never with VM_LOCK_ANONYMITY */
    char usb_auto[VM_USB_AUTO][VM_USB_RULE_MAX + 1];
    int n_usb_auto;
    /* schema 21: "usb": true, the machine has a USB controller for this computer's devices (6.6): a qemu-xhci of its own on
     * slot 0x15 (the compatible and Windows devices in a window use their xHCI). A device of the guest, so it changes only
     * with no saved state. Never with VM_LOCK_ANONYMITY */
    int usb;
    char created[21], updated[21];      /* UTC, "2026-11-04T20:00:00Z" */
} VmMachine;

/* the computer it runs on */
typedef struct {
    int arch;                           /* VM_ARCH_*, or -1 for another architecture */
    int cpus;
    long ram_mb;
    int kvm;                            /* /dev/kvm can be opened */
    int gicv3;                          /* arm64: the interrupt controller is GICv3 (or v4), so KVM can give a guest a GICv3;
                                         * the Raspberry Pi 5's GIC-400 is GICv2 (measured: "KVM does not support GICv3
                                         * emulation") */
} VmHost;

enum {
    VM_OK = 0,
    VM_E_JSON,          /* not JSON, or not an object */
    VM_E_NEWER,         /* schema newer than this build: read-only */
    VM_E_SCHEMA,        /* missing or unknown schema */
    VM_E_FIELD,         /* unknown key, wrong type or a required key missing */
    VM_E_RANGE,         /* a value outside its limits */
    VM_E_HOST,          /* doesn't fit this computer */
    VM_E_IO,
};

/* a share's mount tag: 1 to VM_TAG_MAX of [A-Za-z0-9_-] */
int vm_tag_valid(const char *t);
/* whether a folder may be shared with a guest: an existing directory, given as its real path (no symbolic link on the
 * way), inside home (not home itself) or under /media/<user> or /mnt, with no hidden component (".config", ".ssh"...),
 * and neither inside nor holding the machines' library (a guest could otherwise rewrite its own machine.json) nor the
 * machine's own folder. home and library are real paths. 0 allowed, else -1 and why in err */
int vm_share_allowed(const char *path, const char *home, const char *user, const char *library, char *err, size_t errlen);
/* the transfer folder of the machine in `dir` (6.7.1): <library>/Transfer/<folder name without .lordvm>, where the library
 * is the folder that holds `dir`. 0, or -1 when the machine's folder name can't give one (not <name>.lordvm, hidden, too
 * long) */
int vm_transfer_path(const char *dir, char *out, size_t n);
/* the transfer folder of the machine `id` in `dir`, made when it isn't there (<library>/Transfer 0700, the folder 0777)
 * and tied to the machine: its owner record, <library>/Transfer/.<name>.machine, holds the machine's id. A folder there
 * that another machine owns (a deleted, renamed or remade machine of the same name), or that nobody owns and isn't
 * empty, is moved aside to "<name> (old YYYY-MM-DD)" (its new name in moved), never deleted. 0 and the path in out,
 * else -1 */
int vm_transfer_claim(const char *dir, const char *id, char *out, size_t n, char *moved, size_t mn);
/* the machine goes (to the Trash): its transfer folder, when it's this machine's, moves aside as above. 0 moved, 1 there
 * was none of this machine's, -1 it couldn't be moved */
int vm_transfer_aside(const char *dir, const char *id, char *moved, size_t mn);
/* this machine's transfer folder under another name, left behind when its machine folder was renamed (by hand, or by a
 * rename that stopped between its two steps): one <library>/Transfer/<x> whose owner record holds `id`, whose
 * <library>/<x>.lordvm is gone (a Duplicate shares the id: its folder is still there) and that isn't a folder put aside
 * ("<x> (old YYYY-MM-DD)"), moved to dir's transfer path with its record when that path is free. Exactly one such folder,
 * never a guess. 0 moved (its old name in from), 1 nothing to move, -1 the move failed. vm_transfer_claim calls it first */
int vm_transfer_adopt(const char *dir, const char *id, char *from, size_t fn);

/* Renaming a machine (docs/VIRTUALIZATION.md 6.26): its name, and its folder <library>/<folder>.lordvm and transfer folder
 * <library>/Transfer/<folder> with it, while it's off. */
/* the folder a name gives (as a new machine gets it): letters, digits, ' ', '-', '_' and '.' (not first), anything else
 * one '-', at most 48 bytes, none of ' ', '-', '.' at the end; "Machine" when nothing is left. out: at least 64 bytes */
void vm_folder_name(const char *name, char *out, size_t n);
/* a name a machine may be renamed to: vm_name_valid (1 to VM_NAME_MAX bytes of UTF-8, no control characters, no '/' or
 * '\\', not only spaces) and not starting with '.' */
int vm_rename_name_ok(const char *s);
enum { VM_RN_OK = 0, VM_RN_BAD_NAME, VM_RN_NAME_TAKEN, VM_RN_FOLDER_TAKEN, VM_RN_TRANSFER_TAKEN, VM_RN_NOT_MACHINE };
typedef struct {
    int problem;                        /* VM_RN_* */
    int moves;                          /* the folder gets a new name (its name, made safe, says something else now) */
    char folder[256];                   /* the folder's name then, without .lordvm (the same as now when !moves) */
    char other[VM_NAME_MAX + 1];        /* VM_RN_NAME_TAKEN: the other machine's name */
} VmRenamePlan;
/* what renaming the machine in dir (<library>/<x>.lordvm) to name would do, read from the library as it is: refused
 * (problem) for a bad name, another machine of the library with that name (ASCII case ignored), or, when the folder moves,
 * anything in the library called <folder>.lordvm or a folder <library>/Transfer/<folder> other than its own (case ignored
 * both). Returns problem. Read-only: lord-vm checks it again under the machine's lock */
int vm_rename_plan(const char *dir, const char *name, VmRenamePlan *p);

/* a sound device's name as machine.json keeps it (sound_out, sound_in): 1 to VM_SOUND_DEV_MAX printable ASCII characters
 * other than space (a PipeWire node.name such as "alsa_output.pci-0000_00_1f.3.analog-stereo"); 1 valid */
int vm_sound_dev_valid(const char *s);
/* VM_SOUND's text "OUT\tIN" (each "" for the default, or valid as above) into out and in; 0 ok, -1 refused (both kept) */
int vm_sound_devs_parse(const char *text, char *out, size_t on, char *in, size_t inn);
/* a network interface's name as Linux allows it: 1 to 15 of [A-Za-z0-9_.-], not "." or ".." */
int vm_ifname_valid(const char *s);
/* a display size a machine may start with: 640x480 to 7680x4320, both even; "WxH" → 0, else -1 */
int vm_resolution_ok(int w, int h);
int vm_resolution_parse(const char *s, int *w, int *h);
/* the forwarded ports' rules: proto, a host port VM_PORT_HOST_MIN..65535, a guest port 1..65535, open; at most
 * VM_PORTS; one rule per protocol and host port; host-only machines open ports to this computer only. 0 ok, else -1 and
 * why in err (English) */
int vm_ports_check(const VmPort *p, int n, int network, char *err, size_t errlen);
/* one rule from the command line's text, "tcp|udp:HOST:GUEST[:this-computer|lan]" (this-computer when left out); 0 ok */
int vm_port_parse(const char *s, VmPort *p);
/* the same rule as that text (out: at least 40 bytes) */
void vm_port_spec(const VmPort *p, char *out, size_t n);
/* passt's forwarding options for the machine's rules of one protocol: "-t"/"-u" values like "127.0.0.1/2222:22", in
 * out[] (each 48 bytes); "none" when there are none. Returns how many values */
int vm_passt_ports(const VmMachine *m, int proto, char out[][48], int max);
/* the name of a bridged machine's macvtap: "lvm" and the first 12 hex digits of its id (15 characters); -1 for a bad id */
int vm_bridge_ifname(const char *id, char out[VM_IFNAME_MAX + 1]);

/* the command line's boot order: "auto", or 1 to 3 different names of VM_BOOT_NAMES joined by commas ("network,disk").
 * Returns how many (0 for auto) with them in order[], or -1 */
int vm_boot_parse(const char *s, int order[VM_BOOT_DEVS]);
/* the same as that text: "auto" or "network,disk" (out: at least 24 bytes) */
void vm_boot_spec(const VmMachine *m, char *out, size_t n);
/* QEMU's bootindex for each device (VM_BOOT_*), counting from first (1 while an installer's kernel has 0), or -1 for none. Auto: the system disk, then the CD/DVD drive, the network card none */
void vm_boot_indexes(const VmMachine *m, int first, int idx[VM_BOOT_DEVS]);

/* parses machine.json text; err (optional) gets the reason in English. VM_OK or VM_E_* */
int vm_machine_parse(const char *text, size_t len, VmMachine *m, char *err, size_t errlen);
/* the checks that hold everywhere (parse calls it; call it again after changing fields) */
int vm_machine_check(const VmMachine *m, char *err, size_t errlen);
/* the checks for this computer: KVM for the same architecture only, vCPUs at most the host's, memory at most
 * RAM minus the desktop's floor. warn (optional) gets advice that doesn't refuse (more vCPUs than recommended). */
int vm_machine_check_host(const VmMachine *m, const VmHost *h, char *err, size_t errlen, char *warn, size_t warnlen);
/* JSON text of m (malloc'd, NUL-terminated); NULL on out of memory */
char *vm_machine_json(const VmMachine *m);
/* reads and parses <dir>/machine.json */
int vm_machine_load(const char *dir, VmMachine *m, char *err, size_t errlen);
/* writes <dir>/machine.json atomically (temporary file, fsync, rename); refuses a newer schema */
int vm_machine_save(const char *dir, const VmMachine *m);

/* defaults for a new machine on this host: vCPUs as vm_vcpus_recommended (at most 4), 2 GB (less on a small
 * host), 20 GB disk, KVM when the host can, UEFI, window, shared network; a new id and MAC from getrandom */
void vm_machine_defaults(VmMachine *m, const VmHost *h, int arch);
/* what the host recommends: all CPUs but one (a Pi 5: 3, section 13.7); at least 1 */
int vm_vcpus_recommended(const VmHost *h);
/* the most memory one machine may have: RAM minus the floor */
long vm_mem_max_mb(const VmHost *h);
/* the machine's memory limit (cgroup memory.max): guest memory plus QEMU's overhead */
long vm_mem_limit_mb(const VmMachine *m);
/* Resource budget (3.7): does a machine of `want_mb` fit beside running machines using `running_mb` in total
 * (each as vm_mem_limit_mb)? 1 yes. */
int vm_budget_fits(const VmHost *h, long running_mb, long want_mb);
/* Resource budget (3.7), what every start path asks (js_vm.c budget_refusal: the window, startDetached, `machines start`,
 * a group's members): the memory the library's active machines hold, each as vm_mem_limit_mb, the folder `exclude`
 * (same device and inode) left out. Active is what run.json says (starting, running, paused, stopping) when
 * alive(its lord-vm's pid) agrees; a machine whose supervisor died holds nothing. The library's top level and a machine
 * group's members one level down (<name>.lordgroup/<member>.lordvm), at most 256 machines; symbolic links are never
 * followed. extra (may be NULL) adds a machine's own share beside its limit (vm_group_session_mb: a disposable session).
 * *n: how many were counted. A run.json or machine.json that can't be read counts nothing. */
/* A machine folder whose run.json says it is active (starting, running, paused, stopping) with its lord-vm alive
 * (alive(pid); NULL: run.json alone): 1, else 0. run.json is read with O_NOFOLLOW, at most 2 KB. */
int vm_dir_active(const char *dir, int (*alive)(long pid));
/* Backups (docs/VIRTUALIZATION.md 6.14): a path relative to the home folder that is a machine's disks folder or saved
 * session ("<...>.lordvm/disks" or "<...>.lordvm/suspend.state", a machine at any depth, a group's members included):
 * the length of its machine folder's part ("<...>.lordvm"), else 0. Backups holds those back while the machine runs:
 * a disk QEMU is writing isn't a consistent copy. */
size_t vm_backup_hold(const char *rel);
long vm_budget_used_mb(const char *lib, const char *exclude, int (*alive)(long pid), long (*extra)(const char *dir, const VmMachine *m), int *n);
/* Graphics 3D (virgl) for this start, the part that needs no probe of the host's files: why a machine that asks for 3D
 * starts with 2D graphics instead, or NULL (lord-vm then checks QEMU's GL modules and the render node). An unattended
 * install's start (installing: lord-vm's disks/unattended) is always 2D: the installer is a text console, and QEMU
 * 10.0-11.1 with -display dbus,gl=on aborts when the guest resets virtio-gpu-gl, which the installed kernel (Debian's
 * 6.12.117 and later: virtio_dev_shutdown) does at the installer's restart (docs/VIRTUALIZATION.md, "Graphics 3D") */
const char *vm_gl_refusal(const VmMachine *m, int host_arch, int installing);

/* reads this computer: arch, CPUs online, RAM, /dev/kvm access */
void vm_host_probe(VmHost *h);
/* 1 when an interrupts table (/proc/interrupts) names a GICv3 or GICv4 controller, 0 when not, -1 when unreadable; read
 * in bounded chunks, at most 8 MB */
int vm_gicv3_in(const char *path);
/* its physical cores (each core's first thread in /sys/devices/system/cpu), at most cpus; cpus when it can't tell */
int vm_host_cores(int cpus);

/* An image to import: its format from its first bytes and its name, never QEMU's probing (a crafted qcow2 can name
 * a backing file or an external data file, and the import would copy that file into the new disk; a raw file can
 * hold a qcow2 header and pretend). head: the file's first bytes (VM_IMG_HEAD, fewer for a short file).
 * VM_IMG_QCOW2: the qcow2 magic, version 2 or 3, no backing file, no external data file, not marked corrupt.
 * VM_IMG_RAW: a .img or .raw name and no qcow2 magic. Anything else: -1, err says why. The same check guards the
 * system disk at every start (vm_qcow2_plain). */
enum { VM_IMG_QCOW2 = 0, VM_IMG_RAW = 1 };
#define VM_IMG_HEAD 512
int vm_image_kind(const uint8_t *head, size_t n, const char *name, char *err, size_t errlen);
/* a qcow2 header lord-vm may hand QEMU: 0 yes (no backing file, no external data file, not corrupt) */
int vm_qcow2_plain(const uint8_t *head, size_t n, char *err, size_t errlen);
/* the same for a linked clone's system disk, which has a backing file: its name in the header is never followed (lord-vm
 * names machine.json's base itself), the other checks stay */
int vm_qcow2_overlay(const uint8_t *head, size_t n, char *err, size_t errlen);
/* a base path machine.json may name: absolute, plain, in a ".shared" folder, a .qcow2 */
int vm_base_valid(const char *s);

/* the disks' file extension: "raw" for a raw machine, else "qcow2" (disks/system.<ext>, data-N.<ext>) */
const char *vm_disk_ext(const VmMachine *m);
/* a key lord-vm accepts for an encrypted machine: exactly 64 lowercase hex digits (32 random bytes made by lord-server;
 * shown to the person as the machine's recovery key, which brings an exported machine in on another computer) */
int vm_key_valid(const char *k, size_t n);

const char *vm_arch_name(int arch);     /* "arm64", "amd64" */
int vm_uuid_valid(const char *s);
/* an auto-connect rule of usb_auto (schema 18): "vvvv:pppp" or "vvvv:pppp:serial" */
int vm_usb_rule_valid(const char *s);
int vm_mac_valid(const char *s);      /* lowercase aa:bb:cc:dd:ee:ff, unicast, not all zero */
int vm_smbios_valid(const char *s);   /* printable ASCII, at most VM_SMBIOS_MAX bytes */
int vm_serial_valid(const char *s);   /* letters, digits, '-', '_', '.', at most VM_SERIAL_MAX */
int vm_name_valid(const char *s);
void vm_now_iso(char out[21]);
/* new random id and MAC */
int vm_new_id(char out[37]);
int vm_new_mac(char out[18]);

/* "Capture mouse" (6.25): which of QEMU's mice (query-mice's "return", untrusted: checked here) should be the active one.
 * relative 1: one that isn't absolute, the USB or virtio mouse before the PS/2 one; 0: the tablet (absolute, "Tablet" in
 * its name first). Returns that mouse's index, or -1 when there is none; *current: it is the active one already;
 * *added: the active one is a relative mouse other than PS/2 (one lord-vm added, which took over by itself) */
struct JVal;
int vm_mouse_pick(const struct JVal *list, int relative, int *current, int *added);

#endif
