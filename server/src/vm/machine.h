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
 * smbios, disk_serial, data_disks, rtc. 4 (0.8.8.7): shares. An older file is read as is and written as the current schema, with a copy of the old
 * one kept beside it (machine.json.schema1); a newer one is never rewritten. */
#define VM_SCHEMA 4

enum { VM_ARCH_ARM64 = 0, VM_ARCH_AMD64 };
enum { VM_ACCEL_KVM = 0, VM_ACCEL_TCG };
enum { VM_FW_UEFI = 0, VM_FW_BIOS };
enum { VM_DISPLAY_WINDOW = 0, VM_DISPLAY_NONE };
enum { VM_NET_SHARED = 0, VM_NET_ISOLATED };
/* the shared clipboard's direction (6.8): off unless chosen */
enum { VM_CLIP_OFF = 0, VM_CLIP_TO_GUEST, VM_CLIP_FROM_GUEST, VM_CLIP_BOTH };
/* the disks' and the pointer's devices (6.22): virtio (fast, needs the guest's drivers) or compat, the ones every system has
 * drivers for: SATA (AHCI) disks and CD/DVD, a USB tablet. The network card has its own model (nic_model). */
enum { VM_DEV_VIRTIO = 0, VM_DEV_COMPAT };
/* network card models: virtio-net-pci and the emulated cards an imported machine may have had */
enum { VM_NIC_VIRTIO = 0, VM_NIC_E1000E, VM_NIC_E1000, VM_NIC_E1000_82545EM, VM_NIC_E1000_82544GC, VM_NIC_VMXNET3, VM_NIC_RTL8139, VM_NIC_PCNET, VM_NIC_COUNT };
extern const char *const VM_NIC_MODELS[VM_NIC_COUNT + 1];   /* "virtio", "e1000e", ... (machine.json's names) */
enum { VM_RTC_UTC = 0, VM_RTC_LOCAL };
#define VM_EXTRA_NICS  7            /* cards beyond the first, carried from an import: present with their MAC, not connected */
#define VM_DATA_DISKS  4            /* disks/data-1.qcow2 ... data-4.qcow2 */
#define VM_SMBIOS_MAX  64           /* bytes of printable ASCII per SMBIOS string */
#define VM_SERIAL_MAX  20           /* a disk's serial: ATA's 20 characters */
#define VM_SHARES      4            /* shared folders (6.7), each a vhost-user-fs device on PCI slots 0xb to 0xe */
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
    char created[21], updated[21];      /* UTC, "2026-11-04T20:00:00Z" */
} VmMachine;

/* the computer it runs on */
typedef struct {
    int arch;                           /* VM_ARCH_*, or -1 for another architecture */
    int cpus;
    long ram_mb;
    int kvm;                            /* /dev/kvm can be opened */
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

/* reads this computer: arch, CPUs online, RAM, /dev/kvm access */
void vm_host_probe(VmHost *h);

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

const char *vm_arch_name(int arch);     /* "arm64", "amd64" */
int vm_uuid_valid(const char *s);
int vm_mac_valid(const char *s);      /* lowercase aa:bb:cc:dd:ee:ff, unicast, not all zero */
int vm_smbios_valid(const char *s);   /* printable ASCII, at most VM_SMBIOS_MAX bytes */
int vm_serial_valid(const char *s);   /* letters, digits, '-', '_', '.', at most VM_SERIAL_MAX */
int vm_name_valid(const char *s);
void vm_now_iso(char out[21]);
/* new random id and MAC */
int vm_new_id(char out[37]);
int vm_new_mac(char out[18]);

#endif
