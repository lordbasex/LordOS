/* install_plan_test.c — install-run's plan parser: what must never become an erase by accident. It
 * includes install.c for the static parse_plan; the helper is stubbed, nothing here runs a program. */
#include "install/install.c"

void admin_say(const char *kind, const char *fmt, ...) { (void)kind; (void)fmt; }
int admin_run(const char *const argv[]) { (void)argv; return 1; }
int admin_run_capture(const char *const argv[], char *out, size_t cap) { (void)argv; (void)cap; out[0] = 0; return 1; }

static int fails, checks;
#define T(c) do { checks++; if (!(c)) { fails++; printf("FAILED line %d: %s\n", __LINE__, #c); } } while (0)

int main(void) {
    static Plan p;
    /* the wizard's two plans */
    T(parse_plan("{\"mode\":\"erase\",\"disk\":\"sda\",\"swap\":2048}", &p) == 0 && fsp_mode(p.mode) == 0 && !strcmp(p.disk, "sda") && p.swap == 2048);
    T(parse_plan("{\"mode\":\"free\",\"disk\":\"sda\",\"freeStart\":41943040,\"freeEnd\":209713151}", &p) == 0 && fsp_mode(p.mode) == 1);
    T(p.free_start == 41943040 && p.free_end == 209713151);
    /* above 2^53 a number from JavaScript would be wrong anyway; here it is exact */
    T(parse_plan("{\"freeStart\":281474976710655}", &p) == 0 && p.free_start == 281474976710655ull);
    /* the old plan, without a mode, is an erase as it always was */
    T(parse_plan("{\"disk\":\"sda\"}", &p) == 0 && fsp_mode(p.mode) == 0);

    /* a mode that isn't a plain string doesn't fall through to erase: the plan is refused */
    T(parse_plan("{\"mode\":1,\"disk\":\"sda\"}", &p) != 0);
    T(parse_plan("{\"mode\":true,\"disk\":\"sda\"}", &p) != 0);
    T(parse_plan("{\"mode\":null,\"disk\":\"sda\"}", &p) != 0);
    T(parse_plan("{\"mode\":{},\"disk\":\"sda\"}", &p) != 0);
    T(parse_plan("{\"mode\":[\"free\"],\"disk\":\"sda\"}", &p) != 0);
    T(parse_plan("{\"mo\\u0064e\":1}", &p) != 0);
    T(parse_plan("{\"mode\":\"free\",\"mode\":\"erase\"}", &p) != 0);
    T(parse_plan("{\"mode\":\"erase\",\"mode\":\"free\"}", &p) != 0);
    T(parse_plan("{\"mode\":\"a-mode-much-too-long-for-its-field\"}", &p) != 0);
    /* an unknown mode parses and validate() refuses it first of all, before looking at any disk */
    T(parse_plan("{\"mode\":\"FREE\",\"disk\":\"sda\"}", &p) == 0);
    T(!strcmp(validate(&p), "install.badMode"));
    /* bad region numbers */
    T(parse_plan("{\"mode\":\"free\",\"freeStart\":-5}", &p) != 0);
    T(parse_plan("{\"mode\":\"free\",\"freeEnd\":99999999999999999999999}", &p) != 0);
    printf("install-plan-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
