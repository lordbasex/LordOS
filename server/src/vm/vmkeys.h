/* vmkeys.h — Linux evdev key codes (what lord-server's input gives) to QEMU's "qnum" numbers, which
 * org.qemu.Display1.Keyboard takes: the PC set-1 scan code, with 0x80 added for the E0-prefixed ones. Codes 1–83
 * are the same in both. 0: no such key (dropped). */
#ifndef LORD_VM_KEYS_H
#define LORD_VM_KEYS_H

#include <stdint.h>

static inline uint32_t vm_evdev_to_qnum(int code) {
    if (code >= 1 && code <= 83) return (uint32_t)code;
    switch (code) {
    case 86: return 0x56;           /* KEY_102ND */
    case 87: return 0x57;           /* KEY_F11 */
    case 88: return 0x58;           /* KEY_F12 */
    case 89: return 0x73;           /* KEY_RO */
    case 92: return 0x79;           /* KEY_HENKAN */
    case 93: return 0x70;           /* KEY_KATAKANAHIRAGANA */
    case 94: return 0x7b;           /* KEY_MUHENKAN */
    case 96: return 0x9c;           /* KEY_KPENTER */
    case 97: return 0x9d;           /* KEY_RIGHTCTRL */
    case 98: return 0xb5;           /* KEY_KPSLASH */
    case 99: return 0xb7;           /* KEY_SYSRQ (Print Screen) */
    case 100: return 0xb8;          /* KEY_RIGHTALT */
    case 102: return 0xc7;          /* KEY_HOME */
    case 103: return 0xc8;          /* KEY_UP */
    case 104: return 0xc9;          /* KEY_PAGEUP */
    case 105: return 0xcb;          /* KEY_LEFT */
    case 106: return 0xcd;          /* KEY_RIGHT */
    case 107: return 0xcf;          /* KEY_END */
    case 108: return 0xd0;          /* KEY_DOWN */
    case 109: return 0xd1;          /* KEY_PAGEDOWN */
    case 110: return 0xd2;          /* KEY_INSERT */
    case 111: return 0xd3;          /* KEY_DELETE */
    case 113: return 0xa0;          /* KEY_MUTE */
    case 114: return 0xae;          /* KEY_VOLUMEDOWN */
    case 115: return 0xb0;          /* KEY_VOLUMEUP */
    case 116: return 0xde;          /* KEY_POWER */
    case 117: return 0x59;          /* KEY_KPEQUAL */
    case 119: return 0xc6;          /* KEY_PAUSE */
    case 121: return 0x7e;          /* KEY_KPCOMMA */
    case 124: return 0x7d;          /* KEY_YEN */
    case 125: return 0xdb;          /* KEY_LEFTMETA */
    case 126: return 0xdc;          /* KEY_RIGHTMETA */
    case 127: return 0xdd;          /* KEY_COMPOSE (menu) */
    case 183: return 0x5d;          /* KEY_F13 */
    case 184: return 0x5e;          /* KEY_F14 */
    case 185: return 0x5f;          /* KEY_F15 */
    default: return 0;
    }
}

#endif
