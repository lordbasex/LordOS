/* vmseccomp_other.c — the other architecture's allowlist rows (vmsyscalls.h), for tests/vmseccomp_test.c: built against
 * that architecture's own <asm/unistd_64.h>, so a row the other kernel doesn't have doesn't compile. Nothing of this
 * computer's numbers is included here. Without the other architecture's header: no rows ("skipped"). */
#include "../src/vm/vmseccomp.h"

#include <linux/audit.h>

#if defined(__x86_64__) && __has_include(<aarch64-linux-gnu/asm/unistd_64.h>)
#include <aarch64-linux-gnu/asm/unistd_64.h>
#define OTHER_ARCH AUDIT_ARCH_AARCH64
#define OTHER_NAME "arm64"
#define OTHER_ROWS(X) VM_SC_ARM64(X)
#elif defined(__aarch64__) && __has_include(<x86_64-linux-gnu/asm/unistd_64.h>)
#include <x86_64-linux-gnu/asm/unistd_64.h>
#define OTHER_ARCH AUDIT_ARCH_X86_64
#define OTHER_NAME "amd64"
#define OTHER_ROWS(X) VM_SC_AMD64(X)
#endif

#include "../src/vm/vmsyscalls.h"

#ifdef OTHER_ARCH
#define ROW(n, r) { #n, __NR_##n, VSR_##r },
static const VmScRow OTHER[] = { VM_SC_COMMON(ROW) OTHER_ROWS(ROW) };
#endif

const VmScRow *vmsc_other_rows(int *n, uint32_t *arch, const char **name) {
#ifdef OTHER_ARCH
    *n = (int)(sizeof OTHER / sizeof OTHER[0]);
    *arch = OTHER_ARCH;
    *name = OTHER_NAME;
    return OTHER;
#else
    *n = 0;
    *arch = 0;
    *name = "";
    return 0;
#endif
}
