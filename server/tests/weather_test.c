/* weather_test.c — the weather widget's request path and strict parsing (src/fetch/weather.c): the path
 * is built only from two numbers in range, Open-Meteo's answer keeps numbers only, anything missing or
 * out of range is refused, and lord-server's reading of the helper's compact JSON checks it all again. */
#include "../src/fetch/weather.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0, oks = 0;
#define CHECK(name, cond) do { if (cond) oks++; else { fails++; fprintf(stderr, "FAIL %s\n", name); } } while (0)

static const char *GOOD =
    "{\"latitude\":-34.6,\"longitude\":-58.4,\"current_units\":{\"temperature_2m\":\"°C\"},"
    "\"current\":{\"time\":\"2026-11-16T12:00\",\"interval\":900,\"temperature_2m\":24.3,\"apparent_temperature\":25.1,\"weather_code\":3,\"is_day\":1},"
    "\"daily\":{\"time\":[\"2026-11-16\",\"2026-11-17\",\"2026-11-18\"],\"weather_code\":[3,61,0],"
    "\"temperature_2m_max\":[27.0,22.5,29.1],\"temperature_2m_min\":[17.2,15.0,18.4]}}";

static int parse(const char *s, WxData *d) { return wx_parse(s, strlen(s), d); }

int main(void) {
    char p[256];
    CHECK("path: built from the coordinates", !wx_path(-34.6037, -58.3816, p, sizeof p) && strstr(p, "/v1/forecast?latitude=-34.6037&longitude=-58.3816&") == p);
    CHECK("path: latitude out of range", wx_path(91, 0, p, sizeof p) == -1);
    CHECK("path: longitude out of range", wx_path(0, -181, p, sizeof p) == -1);
    CHECK("path: not a number", wx_path(NAN, 0, p, sizeof p) == -1 && wx_path(0, INFINITY, p, sizeof p) == -1);
    CHECK("path: too small a buffer", wx_path(0, 0, p, 20) == -1);

    WxData d;
    CHECK("parse: a real answer", !parse(GOOD, &d) && d.code == 3 && d.day == 1 && d.ndays == 3 && fabs(d.temp - 24.3) < 1e-9 && d.codes[1] == 61 && fabs(d.lo[2] - 18.4) < 1e-9);
    CHECK("parse: no current", parse("{\"daily\":{\"weather_code\":[1],\"temperature_2m_max\":[1],\"temperature_2m_min\":[1]}}", &d) == -1);
    CHECK("parse: a temperature as text", parse("{\"current\":{\"temperature_2m\":\"24\",\"apparent_temperature\":1,\"weather_code\":3,\"is_day\":1},\"daily\":{\"weather_code\":[1],\"temperature_2m_max\":[1],\"temperature_2m_min\":[1]}}", &d) == -1);
    CHECK("parse: a code out of range", parse("{\"current\":{\"temperature_2m\":1,\"apparent_temperature\":1,\"weather_code\":100,\"is_day\":1},\"daily\":{\"weather_code\":[1],\"temperature_2m_max\":[1],\"temperature_2m_min\":[1]}}", &d) == -1);
    CHECK("parse: a code with decimals", parse("{\"current\":{\"temperature_2m\":1,\"apparent_temperature\":1,\"weather_code\":2.5,\"is_day\":1},\"daily\":{\"weather_code\":[1],\"temperature_2m_max\":[1],\"temperature_2m_min\":[1]}}", &d) == -1);
    CHECK("parse: an impossible temperature", parse("{\"current\":{\"temperature_2m\":500,\"apparent_temperature\":1,\"weather_code\":1,\"is_day\":1},\"daily\":{\"weather_code\":[1],\"temperature_2m_max\":[1],\"temperature_2m_min\":[1]}}", &d) == -1);
    CHECK("parse: a null day", parse("{\"current\":{\"temperature_2m\":1,\"apparent_temperature\":1,\"weather_code\":1,\"is_day\":1},\"daily\":{\"weather_code\":[1,null],\"temperature_2m_max\":[1,2],\"temperature_2m_min\":[1,2]}}", &d) == -1);
    CHECK("parse: no days", parse("{\"current\":{\"temperature_2m\":1,\"apparent_temperature\":1,\"weather_code\":1,\"is_day\":1},\"daily\":{\"weather_code\":[],\"temperature_2m_max\":[],\"temperature_2m_min\":[]}}", &d) == -1);
    CHECK("parse: not JSON", parse("<html>error</html>", &d) == -1);
    CHECK("parse: trailing garbage", parse("{\"current\":{}}x", &d) == -1);
    {
        char big[WX_ANSWER_MAX + 16];
        memset(big, ' ', sizeof big - 1);
        big[sizeof big - 1] = 0;
        CHECK("parse: an answer over the limit", wx_parse(big, strlen(big), &d) == -1);
    }

    char c[1024];
    WxData a, b;
    parse(GOOD, &a);
    size_t n = wx_compact(&a, c, sizeof c);
    CHECK("compact: written", n > 0 && !strncmp(c, "{\"t\":24.3,\"f\":25.1,\"c\":3,\"d\":1,\"days\":[[3,27.0,17.2],", 48));
    CHECK("compact: read back the same", !wx_from_compact(c, n, &b) && b.ndays == 3 && b.codes[1] == 61 && fabs(b.hi[2] - 29.1) < 1e-9 && b.day == 1);
    CHECK("compact: too small a buffer", wx_compact(&a, c, 16) == 0);
    const char *bad[] = {
        "{\"t\":1,\"f\":1,\"c\":1,\"d\":1,\"days\":[]}",
        "{\"t\":1,\"f\":1,\"c\":1,\"d\":1,\"days\":[[1,2]]}",
        "{\"t\":1,\"f\":1,\"c\":1,\"d\":1,\"days\":[[1,2,3]],\"x\":\"<script>\"}x",
        "{\"t\":1,\"f\":1,\"c\":1,\"d\":2,\"days\":[[1,2,3]]}",
        "{\"t\":1,\"f\":1,\"c\":1,\"d\":1,\"days\":[[1,2,3],[1,2,3],[1,2,3],[1,2,3],[1,2,3],[1,2,3],[1,2,3]]}",
        "{\"t\":\"1\",\"f\":1,\"c\":1,\"d\":1,\"days\":[[1,2,3]]}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char nm[64];
        snprintf(nm, sizeof nm, "from_compact: refused #%zu", i);
        CHECK(nm, wx_from_compact(bad[i], strlen(bad[i]), &b) == -1);
    }
    printf(fails ? "weather-test: %d FAILED\n" : "weather-test: %d ok\n", fails ? fails : oks);
    return fails ? 1 : 0;
}
