/* js_hw.h — lord.display, lord.power, lord.bt, lord.mime, lord.timedate (see js_hw.c). */
#ifndef LORD_JS_HW_H
#define LORD_JS_HW_H

#include "quickjs.h"

void js_hw_register(JSContext *c, JSValue lord);

/* How this session composes the screen right now, for lord.sysinfo (in main.c): "gpu", or why it
 * goes by the CPU: "headless" (no monitor), "no-display", "rotated" (a rotated display),
 * "capture" (recording, or a remote viewer with protected windows), "gpu-off" (gpu_info says why). */
const char *lord_composition(void);

#endif
