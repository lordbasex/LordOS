/* vmexec.h — running a program in a guest for the person (docs/VIRTUALIZATION.md 6.15, "lordvm and the agent"):
 * `machines exec`, `machines cp` and the MCP server's run_command, through lord-vm's VM_EXEC.
 *
 * Who may: lord-vm's socket is the person's alone (0700 folder, 0600 socket, SO_PEERCRED both ways), and lord-vm runs
 * a request only for a machine whose machine.json says "automation": true (schema 15; the person's consent, given in
 * Configure; never with the anonymity profile), that runs, whose qemu-guest-agent answers with guest-exec and that is a
 * Linux guest. The program runs as the guest's signed-in person (runuser; the first account from 1000 up without a
 * session), in their home folder, under timeout(1); never as root.
 *
 * The request is a sealed memfd (F_SEAL_WRITE, SHRINK, GROW), at most VMX_REQ_MAX bytes, all integers little endian:
 *   "LVX1"  u32 timeout (seconds, 1..VMX_TIMEOUT_MAX)  u32 argc (1..VMX_ARGC_MAX)
 *   argc × { u32 len (1..VMX_ARG_MAX)  len bytes, no NUL }   (all of them together at most VMX_ARGS_MAX bytes)
 *   u32 inlen (0..VMX_STDIN_MAX)  inlen bytes: the program's standard input
 * and nothing after it. The answer (VM_EXEC_DONE) is a sealed memfd too:
 *   "LVXR"  i32 exit status (0..255; VMX_EXIT_TIMEOUT: timed out, killed)  u32 flags (VMX_F_*)
 *   u32 outlen  outlen bytes of standard output   u32 errlen  errlen bytes of standard error
 * Output is cut at VMX_OUT_MAX and VMX_ERR_MAX in the guest (a flag says so): qemu-guest-agent's answer must fit one
 * line of lord-vm's reader (QMP_LINE_MAX), base64 included.
 *
 * Inside the guest, one fixed script as root finds the person and hands the rest to a second fixed script as that
 * person; the person's script prints "LVXG <exit> <outsize> <errsize>\n", then at most VMX_OUT_MAX bytes of the output
 * and VMX_ERR_MAX of the errors (vmx_guest_parse). Arguments reach the program as arguments, never as script text. */
#ifndef LORD_VM_VMEXEC_H
#define LORD_VM_VMEXEC_H

#include <stddef.h>
#include <stdint.h>

#define VMX_ARGC_MAX     64
#define VMX_ARG_MAX      4096
#define VMX_ARGS_MAX     (64u * 1024)
#define VMX_STDIN_MAX    (1u << 20)
#define VMX_TIMEOUT_MAX  3600
#define VMX_REQ_MAX      (12u + 4u * VMX_ARGC_MAX + VMX_ARGS_MAX + 4u + VMX_STDIN_MAX)
#define VMX_OUT_MAX      (128u * 1024)
#define VMX_ERR_MAX      (32u * 1024)
#define VMX_RES_MAX      (20u + VMX_OUT_MAX + VMX_ERR_MAX)
#define VMX_EXIT_TIMEOUT 124            /* timeout(1)'s own status when the time ran out */

enum { VMX_F_OUT_CUT = 1, VMX_F_ERR_CUT = 2, VMX_F_TIMEOUT = 4 };

typedef struct {
    unsigned timeout;
    int argc;
    const char *argv[VMX_ARGC_MAX + 1];      /* NUL-terminated copies in `store` */
    const uint8_t *in;                       /* into the request */
    size_t inlen;
    char store[VMX_ARGS_MAX + VMX_ARGC_MAX];
} VmxReq;

typedef struct {
    int exit;
    unsigned flags;
    const uint8_t *out, *err;                /* into the buffer parsed */
    size_t outlen, errlen;
} VmxRes;

/* the request's bytes (malloc'd, *len) for argv (argc items) and stdin; NULL when anything is out of range */
uint8_t *vmx_req_encode(unsigned timeout, int argc, const char *const *argv, const uint8_t *in, size_t inlen, size_t *len);
/* 0 and *r filled, or -1: anything out of range, short, long or with a NUL in an argument */
int vmx_req_parse(const uint8_t *b, size_t n, VmxReq *r);

uint8_t *vmx_res_encode(const VmxRes *r, size_t *len);
int vmx_res_parse(const uint8_t *b, size_t n, VmxRes *r);

/* what the person's script printed in the guest (hostile: the guest can print anything) → *r (out and err point into
 * b). 0, or -1 when it isn't exactly a header and at most what it announced, cut at the caps */
int vmx_guest_parse(const uint8_t *b, size_t n, VmxRes *r);

/* the guest's two fixed scripts: as root ($1 the person's script, $2 the timeout, then the program and its arguments),
 * and as the person */
extern const char VMX_ROOT_SH[];
extern const char VMX_PERSON_SH[];

/* a sealed memfd holding n bytes (-1 on failure), and the bytes of one: a memfd sealed against writing, shrinking and
 * growing, of 1..max bytes (malloc'd, NULL otherwise) */
int vmx_memfd(const char *name, const uint8_t *b, size_t n);
uint8_t *vmx_memfd_read(int fd, size_t max, size_t *len);

/* a guest path for `cp` (the person's own words, sent as an argument, never as script text): 1..VMX_ARG_MAX bytes of
 * UTF-8 without control characters. 1 yes */
int vmx_guest_path_ok(const char *p);

#endif
