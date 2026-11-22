/* fpakas_test.c — Flathub's AppStream to the catalog index (src/fpakas.c): what is kept, languages, entities, the
 * developer's name never taken for the app's, other arches and runtimes left out, broken input, and every line checked
 * again by fpakas_line. Also run over random bytes and over cut documents. */
#include "../src/fpakas.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pass, failc;
#define CHECK(x) do { if (x) pass++; else { failc++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

static char got[65536];
static size_t gl;
static int lines_ok, descs_ok, descs_bad;
static char desc[FPAKAS_DESC_MAX + 1], desc_id[64];     /* the last description line, decoded */
static int collect(void *ud, const char *line, size_t n) {
    (void)ud;
    FpakasApp a;
    if (n >= 3 && !memcmp(line, "#d\t", 3)) {
        char id[64], text[FPAKAS_DESC_MAX + 1];
        int l = fpakas_desc_line(line, n, id, text);
        if (l > 0 && (size_t)l == strlen(text)) { descs_ok++; memcpy(desc, text, (size_t)l + 1); memcpy(desc_id, id, sizeof id); }
        else descs_bad++;
    } else if (fpakas_line(line, n, &a) == 0) lines_ok++;
    if (gl + n < sizeof got) { memcpy(got + gl, line, n); gl += n; got[gl] = 0; }
    return 0;
}
static int run(const char *xml, const char *lang) {
    gl = 0; got[0] = 0; lines_ok = descs_ok = descs_bad = 0; desc[0] = desc_id[0] = 0;
    return fpakas_index(xml, strlen(xml), lang, "x86_64", collect, NULL);
}
/* the description of a one-app document whose <component> holds body */
static const char *desc_of_body(const char *body, const char *lang) {
    static char xml[200000];
    snprintf(xml, sizeof xml, "<components><component type=\"desktop-application\"><name>App</name>%s"
             "<bundle type=\"flatpak\">app/org.example.App/x86_64/stable</bundle></component></components>", body);
    run(xml, lang);
    return desc;
}

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

    /* ---- descriptions ---- */
    run(doc, "en");                                             /* the calculator's, CDATA left out; Tabby has none */
    CHECK(descs_ok == 1 && descs_bad == 0 && !strcmp(desc_id, "org.gnome.Calculator") && !strcmp(desc, "Long text"));
    CHECK(strstr(got, "\t1\n#d\torg.gnome.Calculator\tLong text\n") != NULL);   /* right after its app's line */
    /* markup flattened: paragraphs apart, items on their own line, inline markup gone, spaces collapsed */
    CHECK(!strcmp(desc_of_body("<description>\n  <p>First  <em>big</em>\n  para<code>graph</code>.</p>\n"
                               "  <p>Second.</p><ul><li>One</li>\n<li> Two </li></ul><p>After.</p><ol><li>A</li><li>B</li></ol></description>", "en"),
                  "First big paragraph.\n\nSecond.\n\n\xe2\x80\xa2 One\n\xe2\x80\xa2 Two\n\nAfter.\n\n1. A\n2. B"));
    /* entities decoded; a backslash and a line break survive the escaping */
    CHECK(!strcmp(desc_of_body("<description><p>Fish &amp; chips &lt;3 &quot;q&quot; &#233;&#x263A; C:\\dir &unknown; a&b</p></description>", "en"),
                  "Fish & chips <3 \"q\" \xc3\xa9\xe2\x98\xba C:\\dir &unknown; a&b"));
    CHECK(strstr(got, "C:\\\\dir") != NULL);
    /* languages: es-AR → es_AR → es → the untranslated one; never another language */
    const char *langs = "<description><p>English</p></description><description xml:lang=\"de\"><p>Deutsch</p></description>"
                        "<description xml:lang=\"es\"><p>Español</p></description><description xml:lang=\"es_AR\"><p>Argentino</p></description>";
    CHECK(!strcmp(desc_of_body(langs, "es-AR"), "Argentino"));
    CHECK(!strcmp(desc_of_body(langs, "es-MX"), "Español"));
    CHECK(!strcmp(desc_of_body(langs, "fr"), "English"));
    CHECK(!strcmp(desc_of_body("<description xml:lang=\"de\"><p>Nur Deutsch</p></description>", "en"), ""));
    CHECK(descs_ok == 0 && lines_ok == 1);                     /* no line when there is none */
    /* translations per paragraph inside one description: the person's paragraphs, else the untranslated ones */
    const char *perp = "<description><p>Hello</p><p xml:lang=\"es\">Hola</p><ul><li>Item</li><li xml:lang=\"es\">Ítem</li></ul></description>";
    CHECK(!strcmp(desc_of_body(perp, "es"), "Hola\n\n\xe2\x80\xa2 \xc3\x8dtem"));
    CHECK(!strcmp(desc_of_body(perp, "fr"), "Hello\n\n\xe2\x80\xa2 Item"));
    /* a release's description is never the app's */
    CHECK(!strcmp(desc_of_body("<releases><release version=\"1\"><description><p>Bug fixes</p></description></release></releases>", "en"), ""));
    CHECK(!strcmp(desc_of_body("<releases><release><description><p>Fixes</p></description></release></releases><description><p>Real</p></description>", "en"), "Real"));
    /* control characters, tabs and invalid UTF-8 never reach the text */
    CHECK(!strcmp(desc_of_body("<description><p>a\x01" "b\tc\x7f" "d\xff\xfe" "e\xc3(f &#1;g &#x7;h</p></description>", "en"), "a b c de(f g h"));
    /* empty markup: no line */
    desc_of_body("<description><p> </p><ul><li></li></ul></description>", "en");
    CHECK(descs_ok == 0);
    /* the cap: at most FPAKAS_DESC_MAX bytes, cut at a word with "…" */
    {
        static char big[120000];
        size_t o = (size_t)snprintf(big, sizeof big, "<description><p>");
        for (int i = 0; i < 3000; i++) o += (size_t)snprintf(big + o, sizeof big - o, "word%d ", i);
        snprintf(big + o, sizeof big - o, "</p></description>");
        const char *d = desc_of_body(big, "en");
        size_t dl = strlen(d);
        CHECK(descs_ok == 1 && dl <= FPAKAS_DESC_MAX && dl > FPAKAS_DESC_MAX - 16);
        CHECK(dl >= 4 && !memcmp(d + dl - 3, "\xe2\x80\xa6", 3) && d[dl - 4] >= '0' && d[dl - 4] <= '9');   /* "word123…" */
        /* one word longer than the cap: cut on a character */
        o = (size_t)snprintf(big, sizeof big, "<description><p>");
        for (int i = 0; i < 3000; i++) o += (size_t)snprintf(big + o, sizeof big - o, "\xc3\xa9");
        snprintf(big + o, sizeof big - o, "</p></description>");
        d = desc_of_body(big, "en");
        dl = strlen(d);
        CHECK(descs_ok == 1 && dl <= FPAKAS_DESC_MAX && !memcmp(d + dl - 3, "\xe2\x80\xa6", 3));
        /* a cut list never ends on a lone bullet */
        o = (size_t)snprintf(big, sizeof big, "<description><ul>");
        for (int i = 0; i < 2000; i++) o += (size_t)snprintf(big + o, sizeof big - o, "<li>x</li>");
        snprintf(big + o, sizeof big - o, "</ul></description>");
        d = desc_of_body(big, "en");
        dl = strlen(d);
        CHECK(descs_ok == 1 && dl <= FPAKAS_DESC_MAX && !memcmp(d + dl - 4, "x\xe2\x80\xa6", 4));
        /* deep nesting inside a description: the index ends there, nothing bad comes out */
        o = (size_t)snprintf(big, sizeof big, "<description>");
        for (int i = 0; i < 5000; i++) o += (size_t)snprintf(big + o, sizeof big - o, "<p>");
        snprintf(big + o, sizeof big - o, "deep</description>");
        desc_of_body(big, "en");
        CHECK(descs_bad == 0);
        /* nesting a few levels deep, in another language */
        CHECK(!strcmp(desc_of_body("<description><p xml:lang=\"de\"><em><p>nein</p></em>still no</p><p>yes</p></description>", "en"), "yes"));
    }
    /* description lines that must not pass lord-server's check */
    {
        char id[64], text[FPAKAS_DESC_MAX + 1];
        const char *dbad[] = {
            "#d\torg.x.Y\t\n",                                  /* empty */
            "#d\torg.x\tText\n",                                /* a bad id */
            "#d\torg.x.Y\tTab\there\n",                         /* a raw tab */
            "#d\torg.x.Y\tBell\x07\n",                          /* a control character */
            "#d\torg.x.Y\tBad \\t escape\n",                    /* only \\n and \\\\ */
            "#d\torg.x.Y\tTrailing \\\n",                       /* a lone backslash */
            "#d\torg.x.Y\tBad \xff UTF-8\n",
            "#d\torg.x.Y\tCut \xc3\n",
            "#d\torg.x.Y\t\\nleading break\n",
            "#d\torg.x.Y\ttrailing break\\n\n",
            "#d\torg.x.Y\ttoo\\n\\n\\nmany\n",
            "#x\torg.x.Y\tText\n",
            "#d\torg.x.Y\n",
        };
        for (size_t i = 0; i < sizeof dbad / sizeof dbad[0]; i++)
            if (fpakas_desc_line(dbad[i], strlen(dbad[i]), id, text) >= 0) { failc++; printf("FAIL bad description line %zu passed\n", i); } else pass++;
        /* over the cap once decoded */
        static char huge[FPAKAS_DESC_LINE_MAX + 64];
        size_t o = (size_t)snprintf(huge, sizeof huge, "#d\torg.x.Y\t");
        while (o < 3 + 8 + 1 + FPAKAS_DESC_MAX + 1) huge[o++] = 'a';
        huge[o++] = '\n';
        CHECK(fpakas_desc_line(huge, o, id, text) < 0);
        o = (size_t)snprintf(huge, sizeof huge, "#d\torg.x.Y\t");
        while (o < sizeof huge - 1) huge[o++] = 'a';
        CHECK(fpakas_desc_line(huge, o, id, text) < 0);          /* longer than any line */
        const char *dgood = "#d\torg.x.Y\tOne \\\\ two\\n\\n\xe2\x80\xa2 three\n";
        CHECK(fpakas_desc_line(dgood, strlen(dgood), id, text) == 20 && !strcmp(id, "org.x.Y") && !strcmp(text, "One \\ two\n\n\xe2\x80\xa2 three"));
    }
    /* random markup inside a description: every line it makes passes the check */
    srand(11);
    for (int round = 0; round < 3000; round++) {
        char body[900];
        size_t l = 0;
        l += (size_t)snprintf(body, sizeof body, "<description>");
        int parts = rand() % 40;
        for (int i = 0; i < parts && l < sizeof body - 64; i++) {
            static const char *bits[] = { "<p>", "</p>", "<ul>", "</ul>", "<ol>", "</ol>", "<li>", "</li>", "<em>", "</em>", "<code>",
                                          "</code>", "<p xml:lang=\"es\">", "<li xml:lang=\"de\">", "&amp;", "&#0;", "&#xD800;", "\\", "\n",
                                          "\t", "\x01", "\xff", "\xe2\x80", "word ", "  ", "<!-- c -->", "<![CDATA[x]]>", "<br/>", "<", ">", "&" };
            const char *b = bits[rand() % (int)(sizeof bits / sizeof bits[0])];
            l += (size_t)snprintf(body + l, sizeof body - l, "%s", b);
        }
        snprintf(body + l, sizeof body - l, "</description>");
        desc_of_body(body, rand() % 2 ? "es" : "en");
        if (descs_bad) { failc++; printf("FAIL random description %d made a bad line: %s\n", round, body); break; }
    }
    CHECK(1);
    printf("fpakas-test: %d passed, %d failed\n", pass, failc);
    return failc != 0;
}
