/* fpakas_test.c — Flathub's AppStream to the catalog index (src/fpakas.c): what is kept, languages, entities, the
 * developer's name never taken for the app's, other arches and runtimes left out, broken input, and every line checked
 * again by fpakas_line. Also run over random bytes and over cut documents. */
#include "../src/fpakas.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pass, failc;
#define CHECK(x) do { if (x) pass++; else { failc++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

static char got[16384];
static size_t gl;
static int lines_ok;
static int collect(void *ud, const char *line, size_t n) {
    (void)ud;
    FpakasApp a;
    if (fpakas_line(line, n, &a) == 0) lines_ok++;
    if (gl + n < sizeof got) { memcpy(got + gl, line, n); gl += n; got[gl] = 0; }
    return 0;
}
static int run(const char *xml, const char *lang) { gl = 0; got[0] = 0; lines_ok = 0; return fpakas_index(xml, strlen(xml), lang, "x86_64", collect, NULL); }

static const char *doc =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<components version=\"0.8\" origin=\"flathub\">\n"
    "<!-- a comment with <component> inside -->\n"
    "<component type=\"desktop-application\">\n"
    "  <id>org.gnome.Calculator.desktop</id>\n"
    "  <name>Calculator</name>\n"
    "  <name xml:lang=\"es\">Calculadora</name>\n"
    "  <name xml:lang=\"pt_BR\">Calculadora BR</name>\n"
    "  <summary>Perform &amp; check &lt;calculations&gt; &#233;&#x1F600;</summary>\n"
    "  <summary xml:lang=\"es\">Hacer\tcálculos\n  rápido</summary>\n"
    "  <developer><name>The GNOME Project</name></developer>\n"
    "  <description><p>Long <em>text</em> <![CDATA[ <name>Not this</name> ]]></p></description>\n"
    "  <icon type=\"cached\" width=\"128\" height=\"128\">org.gnome.Calculator.png</icon>\n"
    "  <icon type=\"cached\" width=\"64\" height=\"64\">org.gnome.Calculator.png</icon>\n"
    "  <icon type=\"remote\" width=\"64\">https://example.org/x.png</icon>\n"
    "  <categories><category>Utility</category><category>Calculator</category></categories>\n"
    "  <project_license>GPL-3.0-or-later</project_license>\n"
    "  <bundle type=\"flatpak\" runtime=\"org.gnome.Platform/x86_64/48\">app/org.gnome.Calculator/x86_64/stable</bundle>\n"
    "  <custom><value key=\"flathub::verification::verified\">true</value><value key=\"other\">x</value></custom>\n"
    "</component>\n"
    "<component type=\"runtime\"><id>org.gnome.Platform</id><name>GNOME</name>"
    "<bundle type=\"flatpak\">runtime/org.gnome.Platform/x86_64/48</bundle></component>\n"
    "<component type=\"desktop-application\"><id>com.example.Arm</id><name>Arm only</name>"
    "<bundle type=\"flatpak\">app/com.example.Arm/aarch64/stable</bundle></component>\n"
    "<component type=\"desktop-application\"><id>com.example.NoName</id>"
    "<bundle type=\"flatpak\">app/com.example.NoName/x86_64/stable</bundle></component>\n"
    "<component type=\"desktop-application\"><id>bad</id><name>Bad id</name>"
    "<bundle type=\"flatpak\">app/../x86_64/stable</bundle></component>\n"
    "<component type=\"desktop-application\"><id>com.example.Tabby</id><name>Tab\tname</name>"
    "<developer_name>Old Style Dev</developer_name><icon type=\"cached\" width=\"64\">../../etc/passwd.png</icon>"
    "<bundle type=\"flatpak\">app/com.example.Tabby/x86_64/beta</bundle></component>\n"
    "</components>\n";

int main(void) {
    int n = run(doc, "es-AR");
    CHECK(n == 6);                                              /* every component counted */
    CHECK(lines_ok == 2);                                       /* two kept, both pass the check */
    CHECK(strstr(got, "org.gnome.Calculator\tstable\tCalculadora\tHacer cálculos rápido\t64x64/org.gnome.Calculator.png\tUtility;Calculator;\tGPL-3.0-or-later\tThe GNOME Project\t1\n") != NULL);
    CHECK(!strstr(got, "Not this") && !strstr(got, "GNOME\t") && !strstr(got, "Arm only") && !strstr(got, "Bad id"));
    CHECK(strstr(got, "com.example.Tabby\tbeta\tTab name\t\t") != NULL);    /* a tab in a field never splits it */
    CHECK(strstr(got, "Old Style Dev") != NULL);
    run(doc, "en");
    CHECK(strstr(got, "\tCalculator\tPerform & check <calculations> é😀\t") != NULL);
    run(doc, "pt-BR");
    CHECK(strstr(got, "\tCalculadora BR\t") != NULL);
    run(doc, "pt-PT");                                          /* no pt_PT: not pt_BR either, the untranslated one */
    CHECK(strstr(got, "\tCalculator\t") != NULL);
    /* a file that isn't AppStream */
    CHECK(fpakas_index("<html></html>", 13, "en", "x86_64", collect, NULL) < 0);
    /* every prefix of the document: never past the end, never a bad line */
    for (size_t cut = 0; cut < strlen(doc); cut++) {
        char *d = malloc(cut ? cut : 1);
        memcpy(d, doc, cut);
        gl = 0; lines_ok = 0;
        int m = fpakas_index(d, cut, "es", "x86_64", collect, NULL);
        CHECK(m >= -1 && lines_ok <= 2);
        free(d);
    }
    /* random bytes around the markers */
    srand(7);
    for (int round = 0; round < 3000; round++) {
        char buf[600];
        size_t l = (size_t)(rand() % (int)sizeof buf);
        for (size_t i = 0; i < l; i++) {
            static const char alpha[] = "<>/=\"'&;#x0aZ \t\ncomponentnamebundlesummaryapp/x86_64.";
            buf[i] = rand() % 4 ? alpha[rand() % (int)(sizeof alpha - 1)] : (char)(rand() & 0xff);
        }
        memcpy(buf, "<components>", l >= 12 ? 12 : l);
        gl = 0;
        fpakas_index(buf, l, "es", "x86_64", collect, NULL);
    }
    CHECK(1);
    /* lines that must not pass */
    FpakasApp a;
    const char *bad[] = {
        "org.x.Y\tstable\tName\tS\t64x64/../../a.png\t\t\t\t1\n",
        "org.x.Y\tstable\tName\tS\t64x64/a.svg\t\t\t\t1\n",
        "org.x.Y\tstable\tName\tS\t32x32/a.png\t\t\t\t1\n",
        "org.x\tstable\tName\tS\t\t\t\t\t1\n",
        "org.x.Y\t../b\tName\tS\t\t\t\t\t1\n",
        "org.x.Y\tstable\t\tS\t\t\t\t\t1\n",
        "org.x.Y\tstable\tName\tS\t\tUtil ity\t\t\t1\n",
        "org.x.Y\tstable\tName\tS\t\t\t\t\t2\n",
        "org.x.Y\tstable\tName\tS\t\t\t\t\t1\textra\n",
        "org.x.Y\tstable\tNa\x01me\tS\t\t\t\t\t1\n",
        "org.x.Y\tstable\tName\tS\t\t\t\t\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) if (fpakas_line(bad[i], strlen(bad[i]), &a) == 0) { failc++; printf("FAIL bad line %zu passed\n", i); } else pass++;
    const char *good = "org.x.Y\tstable\tName\tSum\t128x128/org.x.Y.png\tGame;\tMIT\tDev\t0\n";
    CHECK(fpakas_line(good, strlen(good), &a) == 0 && !strcmp(a.icon, "128x128/org.x.Y.png") && !a.verified && !strcmp(a.name, "Name"));
    printf("fpakas-test: %d passed, %d failed\n", pass, failc);
    return failc != 0;
}
