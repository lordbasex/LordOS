/* js_cloud.h — lord.cloud (Cloud; see js_cloud.c). */
#ifndef LORD_JS_CLOUD_H
#define LORD_JS_CLOUD_H

#include "quickjs.h"

void js_cloud_register(JSContext *c, JSValue lord);
int  js_cloud_start(const char *home);     /* the cloud thread, with the keyring for secrets */
void js_cloud_dispatch(void);              /* cloud_fd() is readable: state, link answers, the sign-in page */

#endif
