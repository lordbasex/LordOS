/* clean_admin_stub.c — clean_test links the system actions' folder rules without the admin helper. */
#include "admin.h"
#include <stdarg.h>
void admin_say(const char *kind, const char *fmt, ...) { (void)kind; (void)fmt; }
int admin_run(const char *const argv[]) { (void)argv; return 0; }
