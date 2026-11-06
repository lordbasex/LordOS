/* logclean.h — a log shown in the Machines app (lord.vm.log): the serial console's escape sequences and control
 * characters taken out, in place. Bounded by the string's own end: an ESC or a CSI cut off at the end stops there. */
#ifndef LORD_VM_LOGCLEAN_H
#define LORD_VM_LOGCLEAN_H

static inline void vm_log_clean(char *s) {
    char *w = s;
    for (char *q = s; *q; q++) {
        if (*q == 0x1b) {
            if (!q[1]) break;                       /* a bare ESC at the end */
            if (q[1] == '[') {
                q += 2;                             /* parameters up to the final byte (@..~), never past the end */
                while (*q && !(*q >= '@' && *q <= '~')) q++;
                if (!*q) break;
            } else q++;                             /* ESC and one more character */
            continue;
        }
        if (*q == '\r') continue;
        *w++ = (unsigned char)*q < 0x20 && *q != '\n' ? ' ' : *q;
    }
    *w = 0;
}

#endif
