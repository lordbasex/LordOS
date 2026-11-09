/* js_sftp.h — lord.sftp: the Terminal's Files mode (see js_sftp.c and docs/TERMINAL.md). */
#ifndef LORD_JS_SFTP_H
#define LORD_JS_SFTP_H

#include "quickjs.h"

void js_sftp_register(JSContext *c, JSValue lord);

#endif
