/* js_sql.h — lord.sql: MySQL and MariaDB through lord-sql (see js_sql.c and docs/DATABASES.md). */
#ifndef LORD_JS_SQL_H
#define LORD_JS_SQL_H

#include "quickjs.h"

void js_sql_register(JSContext *c, JSValue lord);

#endif
