/* weather.h — the weather widget's forecast (0.8.8.10, Lord Workspaces; maintainer's approval 2026-11-16):
 * what lord-fetch asks Open-Meteo and the little it keeps of the answer. Pure functions, no I/O
 * (tests/weather_test.c).
 *
 * The request is fixed: host WX_HOST, port 443, one GET of wx_path(), built here from two numbers that
 * lord-server took from tzdata's zone1970.tab (the time zone's own coordinates: no GPS, no address,
 * nothing typed). The answer is parsed strictly into WxData: numbers only, each in a sane range, at most
 * WX_DAYS days; nothing else of it (no text, no URL) reaches lord-server or the desktop. lord-server reads
 * the helper's compact JSON with wx_from_compact, which checks every value again. */
#ifndef LORD_FETCH_WEATHER_H
#define LORD_FETCH_WEATHER_H

#include <stddef.h>

#define WX_HOST "api.open-meteo.com"
#define WX_DAYS 6
#define WX_ANSWER_MAX (64 * 1024)

typedef struct {
    double temp;                /* °C now */
    double feels;               /* °C, apparent */
    int code;                   /* WMO weather code (0..99) */
    int day;                    /* 1 daytime, 0 night */
    int ndays;                  /* 1..WX_DAYS */
    int codes[WX_DAYS];
    double hi[WX_DAYS], lo[WX_DAYS];
} WxData;

/* 0 if lat (-90..90) and lon (-180..180) are numbers; out gets "/v1/forecast?latitude=…" (4 decimals). */
int wx_path(double lat, double lon, char *out, size_t n);
/* Open-Meteo's JSON → d. 0 ok; -1 if anything is missing, of another type or out of range. */
int wx_parse(const char *json, size_t len, WxData *d);
/* d → compact JSON {"t":…,"f":…,"c":…,"d":0|1,"days":[[code,hi,lo],…]} into out; bytes written, 0 if it didn't fit. */
size_t wx_compact(const WxData *d, char *out, size_t n);
/* The compact JSON back (lord-server's side): every value checked again. 0 ok. */
int wx_from_compact(const char *json, size_t len, WxData *d);

#endif
