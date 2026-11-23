/* saytext_test.c — say's text (src/tts/saytext.c): cleaning terminal output, splitting into sentences, telling the
 * language and writing numbers, symbols and abbreviations out as words (all but English). */
#include "tts/saytext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int n, bad;
static void eqs(const char *what, const char *got, size_t glen, const char *want) {
    n++;
    if (glen != strlen(want) || memcmp(got, want, glen)) { bad++; printf("✗ %s\n    got  [%.*s]\n    want [%s]\n", what, (int)glen, got, want); }
}

static void clean(const char *what, const char *in, const char *want) {
    char b[512];
    size_t l = strlen(in);
    memcpy(b, in, l + 1);
    size_t o = say_clean(b, l);
    eqs(what, b, o, want);
}

/* every piece, joined by "|" */
static void split(const char *what, const char *in, size_t max, const char *want) {
    char out[1024] = "";
    size_t len = strlen(in), off = 0, o = 0, skip;
    for (int guard = 0; guard < 100; guard++) {
        size_t k = say_next(in + off, len - off, max, &skip);
        if (!k) break;
        if (o) out[o++] = '|';
        memcpy(out + o, in + off + skip, k);
        o += k;
        off += skip + k;
    }
    eqs(what, out, o, want);
}

static void lang(const char *what, const char *in, const char *fallback, const char *want) {
    const char *got = say_language(in, strlen(in), fallback);
    eqs(what, got, strlen(got), want);
}

/* in, as written by the user of a region ("es_AR"; NULL when unknown) */
static void norm_in(const char *lang, const char *region, const char *in, const char *want) {
    size_t len = 0;
    char *got = say_normalize(in, strlen(in), lang, region, &len);
    char what[300];
    snprintf(what, sizeof what, "%s (%s): %s", lang ? lang : "(none)", region ? region : "no region", in);
    if (!got) { n++; bad++; printf("✗ %s: out of memory\n", what); return; }
    eqs(what, got, len, want);
    if (strlen(got) != len) { bad++; printf("✗ %s: length %zu, NUL at %zu\n", what, len, strlen(got)); }
    free(got);
}

static void norm(const char *lang, const char *in, const char *want) { norm_in(lang, NULL, in, want); }

/* "$" by the user's region (Settings -> Language & Region): pesos where the peso is written "$" */
static void normalize_region(void) {
    static const char *const pesos[] = { "es_AR", "es_MX", "es_CL", "es_CO", "es_UY", "es_DO", "es_CU", NULL };
    for (int i = 0; pesos[i]; i++) norm_in("es", pesos[i], "$10", "diez pesos");
    norm_in("es", "es_AR", "Cuesta $ 1 y 21 $.", "Cuesta un peso y veintiún pesos.");
    norm_in("es", "es_AR.UTF-8", "$1.000.000", "un millón de pesos");
    norm_in("es", "es-MX", "$25,50", "veinticinco pesos con cincuenta");
    norm_in("es", "AR", "$10", "diez pesos");
    norm_in("es", "es_AR", "US$ 10, U$S 10, USD 10 y 10 USD", "diez dólares, diez dólares, diez dólares y diez dólares");
    norm_in("es", "es_AR", "R$ 10 y 10 €", "diez reales y diez euros");
    norm_in("es", "es_US", "$10", "diez dólares");
    norm_in("es", "es_ES", "$10", "diez dólares");
    norm_in("es", "es_EC", "$10", "diez dólares");
    norm_in("es", "es_PR", "$10", "diez dólares");
    norm_in("es", NULL, "$10", "diez dólares");
    norm_in("es", "", "$10", "diez dólares");
    norm_in("es", "es", "$10", "diez dólares");
    norm_in("es", "es.UTF-8", "$10", "diez dólares");
    norm_in("es", "C.UTF-8", "$10", "diez dólares");
    norm_in("es", "es_ARG", "$10", "diez dólares");
    norm_in("pt", "pt_BR", "$10 e R$ 10", "dez dólares e dez reais");
    norm_in("pt", "pt_PT", "$10", "dez dólares");
    norm_in("pt", NULL, "R$ 1", "um real");
    norm_in("pt", "es_AR", "$10 e US$ 10", "dez pesos e dez dólares");
    norm_in("pt", "pt_BR", "USD 1", "um dólar");
    norm_in("en", "es_AR", "$10", "$10");
    norm_in("es", "es_AR", "10 USDT", "diez USDT");
}

static void normalize_es(void) {
    /* the sentence measured with every voice (docs/VOICE.md) */
    norm("es", "Tengo 88 años y nací en 1987. Compré 3 libros por 25 euros. #LordOS es genial.",
         "Tengo ochenta y ocho años y nací en mil novecientos ochenta y siete. Compré tres libros por veinticinco euros. LordOS es genial.");
    /* cardinals */
    norm("es", "0", "cero");
    norm("es", "1", "uno");
    norm("es", "15", "quince");
    norm("es", "16", "dieciséis");
    norm("es", "21", "veintiuno");
    norm("es", "22", "veintidós");
    norm("es", "30", "treinta");
    norm("es", "31", "treinta y uno");
    norm("es", "99", "noventa y nueve");
    norm("es", "100", "cien");
    norm("es", "101", "ciento uno");
    norm("es", "115", "ciento quince");
    norm("es", "200", "doscientos");
    norm("es", "555", "quinientos cincuenta y cinco");
    norm("es", "777", "setecientos setenta y siete");
    norm("es", "999", "novecientos noventa y nueve");
    norm("es", "1000", "mil");
    norm("es", "1001", "mil uno");
    norm("es", "2026", "dos mil veintiséis");
    norm("es", "21000", "veintiún mil");
    norm("es", "100000", "cien mil");
    norm("es", "1000000", "un millón");
    norm("es", "2000000", "dos millones");
    norm("es", "21000000", "veintiún millones");
    norm("es", "123456789", "ciento veintitrés millones cuatrocientos cincuenta y seis mil setecientos ochenta y nueve");
    norm("es", "1.000.000.000", "mil millones");
    norm("es", "1.001.000.000", "mil un millones");
    norm("es", "999.999.999.999", "novecientos noventa y nueve mil novecientos noventa y nueve millones novecientos noventa y nueve mil novecientos noventa y nueve");
    norm("es", "-5 y \xe2\x88\x92" "12", "menos cinco y menos doce");
    /* a count before a noun: un, veintiún, ciento un; not before "de" */
    norm("es", "Tengo 21 años y 1 libro.", "Tengo veintiún años y un libro.");
    norm("es", "Son 31 días y 101 dálmatas.", "Son treinta y un días y ciento un dálmatas.");
    norm("es", "El 1 de enero.", "El uno de enero.");
    norm("es", "En 2001 hubo 21.000 personas.", "En dos mil uno hubo veintiún mil personas.");
    /* thousands separators and decimals */
    norm("es", "1.987", "mil novecientos ochenta y siete");
    norm("es", "1.000.000 de personas", "un millón de personas");
    norm("es", "1 000 casas", "mil casas");
    norm("es", "12 345 678", "doce millones trescientos cuarenta y cinco mil seiscientos setenta y ocho");
    norm("es", "1\xc2\xa0" "000 y 2\xe2\x80\xaf" "500", "mil y dos mil quinientos");
    norm("es", "1,000,000", "un millón");
    norm("es", "3,5 metros", "tres coma cinco metros");
    norm("es", "0,5", "cero coma cinco");
    norm("es", "3,14", "tres coma catorce");
    norm("es", "3,05", "tres coma cero cinco");
    norm("es", "3,14159", "tres coma uno cuatro uno cinco nueve");
    norm("es", "3.5", "tres punto cinco");
    norm("es", "1.234,56", "mil doscientos treinta y cuatro coma cincuenta y seis");
    norm("es", "Versión 0.8.8.10 lista.", "Versión cero punto ocho punto ocho punto diez lista.");
    norm("es", "192.168.0.1", "ciento noventa y dos punto ciento sesenta y ocho punto cero punto uno");
    /* percent */
    norm("es", "25%", "veinticinco por ciento");
    norm("es", "1,5 %", "uno coma cinco por ciento");
    norm("es", "100%", "cien por ciento");
    norm("es", "un % alto", "un por ciento alto");
    /* ordinals */
    norm("es", "1.º", "primero");
    norm("es", "1.ª", "primera");
    norm("es", "2º y 3ª", "segundo y tercera");
    norm("es", "el 1.er puesto y el 3er día", "el primer puesto y el tercer día");
    norm("es", "10.º", "décimo");
    norm("es", "11.º y 13.ª", "undécimo y decimotercera");
    norm("es", "18.º", "decimoctavo");
    norm("es", "21.ª", "vigésima primera");
    norm("es", "45.º", "cuadragésimo quinto");
    norm("es", "100.º", "centésimo");
    norm("es", "250.º", "doscientos cincuenta");
    /* times */
    norm("es", "a las 14:30", "a las catorce y treinta");
    norm("es", "a las 9:05", "a las nueve y cinco");
    norm("es", "a las 14:00", "a las catorce horas");
    norm("es", "a las 14:00 horas", "a las catorce horas");
    norm("es", "a la 1:00 h", "a la una hora");
    norm("es", "a las 21:15 hs", "a las veintiuna y quince");
    norm("es", "14h30", "catorce y treinta");
    norm("es", "48 hs y 4h", "cuarenta y ocho horas y cuatro horas");
    norm("es", "Duró 1:02:03.", "Duró una hora, dos minutos y tres segundos.");
    norm("es", "25:30", "veinticinco: treinta");
    /* dates */
    norm("es", "el 15/11/2026", "el quince de noviembre de dos mil veintiséis");
    norm("es", "el 2026-11-15", "el quince de noviembre de dos mil veintiséis");
    norm("es", "el 1.5.2026", "el uno de mayo de dos mil veintiséis");
    norm("es", "el 31/12/99", "el treinta y uno de diciembre de noventa y nueve");
    /* money */
    norm("es", "25 €", "veinticinco euros");
    norm("es", "€25", "veinticinco euros");
    norm("es", "1 €", "un euro");
    norm("es", "21€", "veintiún euros");
    norm("es", "25,50 €", "veinticinco euros con cincuenta");
    norm("es", "1.000.000 €", "un millón de euros");
    norm("es", "1.500.000 €", "un millón quinientos mil euros");
    norm("es", "$10", "diez dólares");
    norm("es", "US$ 1", "un dólar");
    norm("es", "U$S 100", "cien dólares");
    norm("es", "R$ 10,50", "diez reales con cincuenta");
    norm("es", "£21", "veintiuna libras");
    norm("es", "200 £", "doscientas libras");
    /* degrees */
    norm("es", "Hace -5 °C.", "Hace menos cinco grados Celsius.");
    norm("es", "1°C y 30ºC", "un grado Celsius y treinta grados Celsius");
    norm("es", "100 °F", "cien grados Fahrenheit");
    norm("es", "un ángulo de 90°", "un ángulo de noventa grados");
    /* symbols */
    norm("es", "#5", "número cinco");
    norm("es", "nº 5, n.º 7 y N° 9", "número cinco, número siete y número nueve");
    norm("es", "# Título", "Título");
    norm("es", "C# y F#", "C# y F#");
    norm("es", "@lordbasex", "arroba lordbasex");
    norm("es", "Juan & María", "Juan y María");
    norm("es", "2+2 = 4", "dos más dos igual a cuatro");
    norm("es", "C++", "C++");
    norm("es", "+54 11", "más cincuenta y cuatro once");
    /* phones and codes digit by digit */
    norm("es", "1155551234", "uno uno cinco cinco cinco cinco uno dos tres cuatro");
    norm("es", "0800", "cero ocho cero cero");
    norm("es", "007", "cero cero siete");
    norm("es", "612 345 678", "seiscientos doce trescientos cuarenta y cinco seiscientos setenta y ocho");
    norm("es", "11-5555-1234", "once, cinco mil quinientos cincuenta y cinco, mil doscientos treinta y cuatro");
    norm("es", "del 10-20 de mayo", "del diez a veinte de mayo");
    /* digits in words */
    norm("es", "MP3", "MP tres");
    norm("es", "4K", "cuatro K");
    norm("es", "H264", "H doscientos sesenta y cuatro");
    norm("es", "abc12345", "abc uno dos tres cuatro cinco");
    norm("es", "COVID-19", "COVID diecinueve");
    norm("es", "1080p", "mil ochenta p");
    /* addresses and e-mails */
    norm("es", "Escribí a info@lordos.org.", "Escribí a info arroba lordos punto org.");
    norm("es", "Mirá https://lordos.org/es/ ahora", "Mirá lordos punto org barra es ahora");
    norm("es", "www.debian.org", "www punto debian punto org");
    norm("es", "user_1@mail-x.com", "user guion bajo uno arroba mail guion x punto com");
    /* slashes, punctuation, lines */
    norm("es", "¿Cuánto es 1/2? Abierto 24/7.", "¿Cuánto es uno barra dos? Abierto veinticuatro barra siete.");
    norm("es", "¿25 €?", "¿veinticinco euros?");
    norm("es", "(3 veces)", "(tres veces)");
    norm("es", "uno\n\n2 dos", "uno\n\ndos dos");
    norm("es", "sin números", "sin números");
    norm("es", "", "");
}

static void normalize_pt(void) {
    norm("pt", "Eu tenho 88 anos e nasci em 1987. Comprei 3 livros por 25 euros. #LordOS é ótimo.",
         "Eu tenho oitenta e oito anos e nasci em mil novecentos e oitenta e sete. Comprei três livros por vinte e cinco euros. LordOS é ótimo.");
    /* cardinals (Brazilian) */
    norm("pt", "0", "zero");
    norm("pt", "1", "um");
    norm("pt", "2", "dois");
    norm("pt", "14", "catorze");
    norm("pt", "16 17 19", "dezesseis dezessete dezenove");
    norm("pt", "21", "vinte e um");
    norm("pt", "99", "noventa e nove");
    norm("pt", "100", "cem");
    norm("pt", "101", "cento e um");
    norm("pt", "123", "cento e vinte e três");
    norm("pt", "200", "duzentos");
    norm("pt", "555", "quinhentos e cinquenta e cinco");
    norm("pt", "1000", "mil");
    norm("pt", "1001", "mil e um");
    norm("pt", "1100", "mil e cem");
    norm("pt", "1200", "mil e duzentos");
    norm("pt", "1234", "mil duzentos e trinta e quatro");
    norm("pt", "2026", "dois mil e vinte e seis");
    norm("pt", "2500", "dois mil e quinhentos");
    norm("pt", "100000", "cem mil");
    norm("pt", "1.000.000", "um milhão");
    norm("pt", "1.200.000", "um milhão e duzentos mil");
    norm("pt", "1.001.000", "um milhão e mil");
    norm("pt", "2.000.300", "dois milhões e trezentos");
    norm("pt", "1.234.567", "um milhão duzentos e trinta e quatro mil quinhentos e sessenta e sete");
    norm("pt", "1.000.000.000", "um bilhão");
    norm("pt", "999.999.999.999", "novecentos e noventa e nove bilhões novecentos e noventa e nove milhões novecentos e noventa e nove mil novecentos e noventa e nove");
    norm("pt", "-5", "menos cinco");
    norm("pt", "Tenho 21 anos e 1 livro.", "Tenho vinte e um anos e um livro.");
    /* separators and decimals */
    norm("pt", "1.987", "mil novecentos e oitenta e sete");
    norm("pt", "1 000", "mil");
    norm("pt", "3,5", "três vírgula cinco");
    norm("pt", "3.14", "três ponto catorze");
    norm("pt", "0,05", "zero vírgula zero cinco");
    norm("pt", "1.234,56", "mil duzentos e trinta e quatro vírgula cinquenta e seis");
    norm("pt", "versão 0.8.8.10", "versão zero ponto oito ponto oito ponto dez");
    /* percent */
    norm("pt", "25%", "vinte e cinco por cento");
    norm("pt", "3,5 %", "três vírgula cinco por cento");
    /* ordinals */
    norm("pt", "1º", "primeiro");
    norm("pt", "1ª", "primeira");
    norm("pt", "2.º", "segundo");
    norm("pt", "7º", "sétimo");
    norm("pt", "9º", "nono");
    norm("pt", "10º", "décimo");
    norm("pt", "11º", "décimo primeiro");
    norm("pt", "22ª", "vigésima segunda");
    norm("pt", "40º", "quadragésimo");
    norm("pt", "3er", "três er");
    /* times */
    norm("pt", "às 14:30", "às catorze e trinta");
    norm("pt", "às 14:00", "às catorze horas");
    norm("pt", "14h30", "catorze e trinta");
    norm("pt", "14h", "catorze horas");
    norm("pt", "1:00", "uma hora");
    norm("pt", "21:15", "vinte e uma e quinze");
    norm("pt", "2:05", "duas e cinco");
    norm("pt", "1:01:01", "uma hora, um minuto e um segundo");
    /* dates */
    norm("pt", "15/11/2026", "quinze de novembro de dois mil e vinte e seis");
    norm("pt", "1/5/2026", "primeiro de maio de dois mil e vinte e seis");
    norm("pt", "2026-03-02", "dois de março de dois mil e vinte e seis");
    /* money */
    norm("pt", "R$ 10,50", "dez reais e cinquenta");
    norm("pt", "R$ 1,00", "um real");
    norm("pt", "R$ 1.500,00", "mil e quinhentos reais");
    norm("pt", "R$ 2.000.000", "dois milhões de reais");
    norm("pt", "25 €", "vinte e cinco euros");
    norm("pt", "US$ 10", "dez dólares");
    norm("pt", "£2", "duas libras");
    norm("pt", "1 £", "uma libra");
    norm("pt", "200 £", "duzentas libras");
    /* degrees */
    norm("pt", "Faz -5 °C.", "Faz menos cinco graus Celsius.");
    norm("pt", "1 °C", "um grau Celsius");
    /* symbols */
    norm("pt", "#5", "número cinco");
    norm("pt", "nº 3", "número três");
    norm("pt", "@lordbasex", "arroba lordbasex");
    norm("pt", "P&D", "P e D");
    norm("pt", "2 + 2 = 4", "dois mais dois igual a quatro");
    /* digit by digit, words */
    norm("pt", "11987654321", "um um nove oito sete seis cinco quatro três dois um");
    norm("pt", "0800", "zero oito zero zero");
    norm("pt", "MP3 e 4K", "MP três e quatro K");
    norm("pt", "de 10-20", "de dez a vinte");
    /* addresses */
    norm("pt", "contato@lordos.org", "contato arroba lordos ponto org");
    norm("pt", "http://exemplo.com.br/a_b-c", "exemplo ponto com ponto br barra a sublinhado b hífen c");
}

static void normalize_fr(void) {
    norm("fr", "J'ai 88 ans et je suis né en 1987. J'ai acheté 3 livres pour 25 euros. #LordOS est génial.",
         "J'ai quatre-vingt-huit ans et je suis né en mille neuf cent quatre-vingt-sept. J'ai acheté trois livres pour "
         "vingt-cinq euros. LordOS est génial.");
    /* cardinals */
    norm("fr", "0", "zéro");
    norm("fr", "1", "un");
    norm("fr", "16", "seize");
    norm("fr", "17", "dix-sept");
    norm("fr", "21", "vingt et un");
    norm("fr", "22", "vingt-deux");
    norm("fr", "61", "soixante et un");
    norm("fr", "70", "soixante-dix");
    norm("fr", "71", "soixante et onze");
    norm("fr", "77", "soixante-dix-sept");
    norm("fr", "80", "quatre-vingts");
    norm("fr", "81", "quatre-vingt-un");
    norm("fr", "90", "quatre-vingt-dix");
    norm("fr", "91", "quatre-vingt-onze");
    norm("fr", "99", "quatre-vingt-dix-neuf");
    norm("fr", "100", "cent");
    norm("fr", "101", "cent un");
    norm("fr", "200", "deux cents");
    norm("fr", "280", "deux cent quatre-vingts");
    norm("fr", "1000", "mille");
    norm("fr", "2026", "deux mille vingt-six");
    norm("fr", "80000", "quatre-vingt mille");
    norm("fr", "200000", "deux cent mille");
    norm("fr", "1 000 000", "un million");
    norm("fr", "200\xe2\x80\xaf" "000\xe2\x80\xaf" "000", "deux cents millions");
    norm("fr", "1.000.000.000", "un milliard");
    norm("fr", "-5", "moins cinq");
    /* a count before a noun and feminine money */
    norm("fr", "21 ans", "vingt et un ans");
    norm("fr", "1 £", "une livre");
    norm("fr", "21 £", "vingt et une livres");
    /* decimals, percent, groups */
    norm("fr", "3,5", "trois virgule cinq");
    norm("fr", "3.5", "trois point cinq");
    norm("fr", "1\xe2\x80\xaf" "234,56", "mille deux cent trente-quatre virgule cinquante-six");
    norm("fr", "25 %", "vingt-cinq pour cent");
    norm("fr", "192.168.0.1", "cent quatre-vingt-douze point cent soixante-huit point zéro point un");
    /* ordinals */
    norm("fr", "le 1er", "le premier");
    norm("fr", "la 1re fois", "la première fois");
    norm("fr", "la 1ère fois", "la première fois");
    norm("fr", "le 2e", "le deuxième");
    norm("fr", "le 2nd", "le second");
    norm("fr", "le 3ème", "le troisième");
    norm("fr", "le 5e", "le cinquième");
    norm("fr", "le 9e", "le neuvième");
    norm("fr", "le 21e", "le vingt et unième");
    norm("fr", "le 80e", "le quatre-vingtième");
    norm("fr", "le 100e", "le centième");
    norm("fr", "le XXIe siècle", "le XXIe siècle");
    /* times */
    norm("fr", "14:30", "quatorze heures trente");
    norm("fr", "14h30", "quatorze heures trente");
    norm("fr", "14 h 30", "quatorze heures trente");
    norm("fr", "14:00", "quatorze heures");
    norm("fr", "1:00", "une heure");
    norm("fr", "21:05", "vingt et une heures cinq");
    norm("fr", "1:02:03", "une heure, deux minutes et trois secondes");
    norm("fr", "4h", "quatre heures");
    /* dates */
    norm("fr", "15/11/2026", "quinze novembre deux mille vingt-six");
    norm("fr", "01/05/2026", "premier mai deux mille vingt-six");
    norm("fr", "le 1 janvier", "le premier janvier");
    norm("fr", "le 14 juillet 1789", "le quatorze juillet mille sept cent quatre-vingt-neuf");
    /* money */
    norm("fr", "25 €", "vingt-cinq euros");
    norm("fr", "1 €", "un euro");
    norm("fr", "25,50 €", "vingt-cinq euros cinquante");
    norm("fr", "1 000 000 €", "un million d'euros");
    norm("fr", "2 000 000 $", "deux millions de dollars");
    norm("fr", "$10", "dix dollars");
    norm("fr", "R$ 10", "dix reais");
    /* degrees */
    norm("fr", "21 °C", "vingt et un degrés Celsius");
    norm("fr", "1°C", "un degré Celsius");
    /* symbols */
    norm("fr", "n° 5", "numéro cinq");
    norm("fr", "#5", "numéro cinq");
    norm("fr", "A & B", "A et B");
    norm("fr", "2+2 = 4", "deux plus deux égale quatre");
    norm("fr", "10-20", "dix à vingt");
    norm("fr", "info@lordos.org", "info arobase lordos point org");
    /* abbreviations */
    norm("fr", "M. Dupont et Mme Martin", "monsieur Dupont et madame Martin");
    norm("fr", "Mlle Durand", "mademoiselle Durand");
    norm("fr", "Dr Martin", "docteur Martin");
    norm("fr", "des fruits, p. ex. des pommes", "des fruits, par exemple des pommes");
    norm("fr", "c.-à-d. demain", "c'est-à-dire demain");
    norm("fr", "pommes, poires, etc. Puis", "pommes, poires, et cetera. Puis");
    norm("fr", "pommes, poires, etc.", "pommes, poires, et cetera.");
    norm("fr", "Mmenard", "Mmenard");
    norm("fr", "le point M.", "le point M.");
    norm("fr", "l'11e édition", "l'onzième édition");
    norm("fr", "l\xe2\x80\x99" "11e édition", "l\xe2\x80\x99onzième édition");
}

static void normalize_de(void) {
    norm("de", "Ich bin 88 Jahre alt und wurde 1987 geboren. Ich kaufte 3 Bücher für 25 Euro. #LordOS ist toll.",
         "Ich bin achtundachtzig Jahre alt und wurde neunzehnhundertsiebenundachtzig geboren. Ich kaufte drei Bücher für "
         "fünfundzwanzig Euro. LordOS ist toll.");
    /* cardinals */
    norm("de", "0", "null");
    norm("de", "1", "eins");
    norm("de", "12", "zwölf");
    norm("de", "17", "siebzehn");
    norm("de", "21", "einundzwanzig");
    norm("de", "30", "dreißig");
    norm("de", "99", "neunundneunzig");
    norm("de", "100", "hundert");
    norm("de", "101", "hunderteins");
    norm("de", "121", "hunderteinundzwanzig");
    norm("de", "1000", "tausend");
    norm("de", "1001", "tausendeins");
    norm("de", "2026", "zweitausendsechsundzwanzig");
    norm("de", "21.000", "einundzwanzigtausend");
    norm("de", "100.000", "hunderttausend");
    norm("de", "1.000.000", "eine Million");
    norm("de", "1.200.000", "eine Million zweihunderttausend");
    norm("de", "2.000.000", "zwei Millionen");
    norm("de", "1.000.000.000", "eine Milliarde");
    norm("de", "-5", "minus fünf");
    /* a year in hundreds; with a separator it is a count */
    norm("de", "im Jahr 1990", "im Jahr neunzehnhundertneunzig");
    norm("de", "1100", "elfhundert");
    norm("de", "1.987", "tausendneunhundertsiebenundachtzig");
    norm("de", "2000", "zweitausend");
    /* a count before a noun (nouns are capitalized) */
    norm("de", "1 Buch", "ein Buch");
    norm("de", "1 und 2", "eins und zwei");
    norm("de", "1 mal", "eins mal");
    /* decimals, percent, groups */
    norm("de", "3,5", "drei Komma fünf");
    norm("de", "1.234,56", "tausendzweihundertvierunddreißig Komma sechsundfünfzig");
    norm("de", "25 %", "fünfundzwanzig Prozent");
    norm("de", "0.8.8.10", "null Punkt acht Punkt acht Punkt zehn");
    /* ordinals: after an article or before a month */
    norm("de", "am 3. Mai", "am dritten Mai");
    norm("de", "der 1. Platz", "der erste Platz");
    norm("de", "den 7. Tag", "den siebten Tag");
    norm("de", "3. Oktober", "dritter Oktober");
    norm("de", "die 20. Ausgabe", "die zwanzigste Ausgabe");
    norm("de", "Er kam um 3. Dann ging er.", "Er kam um drei. Dann ging er.");
    /* times */
    norm("de", "14:30", "vierzehn Uhr dreißig");
    norm("de", "14:30 Uhr", "vierzehn Uhr dreißig");
    norm("de", "14.30 Uhr", "vierzehn Uhr dreißig");
    norm("de", "14:00", "vierzehn Uhr");
    norm("de", "1:00", "ein Uhr");
    norm("de", "um 8 Uhr", "um acht Uhr");
    norm("de", "1:02:03", "eine Stunde, zwei Minuten und drei Sekunden");
    norm("de", "4h", "vier Stunden");
    /* dates */
    norm("de", "15.11.2026", "fünfzehnter November zweitausendsechsundzwanzig");
    norm("de", "am 15.11.2026", "am fünfzehnten November zweitausendsechsundzwanzig");
    norm("de", "am 9. November 1989", "am neunten November neunzehnhundertneunundachtzig");
    /* money */
    norm("de", "25 €", "fünfundzwanzig Euro");
    norm("de", "1 €", "ein Euro");
    norm("de", "25,50 €", "fünfundzwanzig Euro fünfzig");
    norm("de", "1.000.000 €", "eine Million Euro");
    norm("de", "$10", "zehn Dollar");
    norm("de", "1 £", "ein Pfund");
    /* degrees */
    norm("de", "21 °C", "einundzwanzig Grad Celsius");
    norm("de", "1 °C", "ein Grad Celsius");
    /* symbols */
    norm("de", "#5", "Nummer fünf");
    norm("de", "A & B", "A und B");
    norm("de", "2+2 = 4", "zwei plus zwei gleich vier");
    norm("de", "10-20", "zehn bis zwanzig");
    norm("de", "info@lordos.org", "info at lordos Punkt org");
    /* abbreviations */
    norm("de", "z. B. Äpfel", "zum Beispiel Äpfel");
    norm("de", "z.B. Äpfel", "zum Beispiel Äpfel");
    norm("de", "d. h. morgen", "das heißt morgen");
    norm("de", "Nr. 5", "Nummer fünf");
    norm("de", "ca. 20 Leute", "circa zwanzig Leute");
    norm("de", "Äpfel, Birnen usw. Dann", "Äpfel, Birnen und so weiter. Dann");
    norm("de", "Dr. Müller und Prof. Weber", "Doktor Müller und Professor Weber");
    norm("de", "bzw. morgen", "beziehungsweise morgen");
}

static void normalize_it(void) {
    norm("it", "Ho 88 anni e sono nato nel 1987. Ho comprato 3 libri per 25 euro. #LordOS è fantastico.",
         "Ho ottantotto anni e sono nato nel millenovecentottantasette. Ho comprato tre libri per venticinque euro. "
         "LordOS è fantastico.");
    /* cardinals */
    norm("it", "0", "zero");
    norm("it", "1", "uno");
    norm("it", "3", "tre");
    norm("it", "17", "diciassette");
    norm("it", "21", "ventuno");
    norm("it", "23", "ventitré");
    norm("it", "28", "ventotto");
    norm("it", "33", "trentatré");
    norm("it", "100", "cento");
    norm("it", "101", "centouno");
    norm("it", "103", "centotré");
    norm("it", "108", "centotto");
    norm("it", "180", "centottanta");
    norm("it", "1000", "mille");
    norm("it", "1003", "milletré");
    norm("it", "2026", "duemilaventisei");
    norm("it", "23.000", "ventitremila");
    norm("it", "1.000.000", "un milione");
    norm("it", "2.500.000", "due milioni cinquecentomila");
    norm("it", "1.000.000.000", "un miliardo");
    norm("it", "-5", "meno cinque");
    /* a count before a noun */
    norm("it", "1 libro", "un libro");
    norm("it", "1 e 2", "uno e due");
    /* decimals, percent */
    norm("it", "3,5", "tre virgola cinque");
    norm("it", "1.234,56", "milleduecentotrentaquattro virgola cinquantasei");
    norm("it", "25%", "venticinque per cento");
    /* ordinals */
    norm("it", "1º", "primo");
    norm("it", "1ª", "prima");
    norm("it", "il 10º", "il decimo");
    norm("it", "l'11º", "l'undicesimo");
    norm("it", "il 23º", "il ventitreesimo");
    norm("it", "il 26º", "il ventiseiesimo");
    norm("it", "la 21ª", "la ventunesima");
    norm("it", "il 100º", "il centesimo");
    /* times */
    norm("it", "14:30", "quattordici e trenta");
    norm("it", "alle 14:00", "alle quattordici");
    norm("it", "1:02:03", "una ora, due minuti e tre secondi");
    norm("it", "4h", "quattro ore");
    /* dates */
    norm("it", "15/11/2026", "quindici novembre duemilaventisei");
    norm("it", "01/05/2026", "primo maggio duemilaventisei");
    norm("it", "il 1 maggio", "il primo maggio");
    /* money */
    norm("it", "25 €", "venticinque euro");
    norm("it", "1 €", "un euro");
    norm("it", "25,50 €", "venticinque euro e cinquanta");
    norm("it", "1.000.000 €", "un milione di euro");
    norm("it", "$10", "dieci dollari");
    norm("it", "1 £", "una sterlina");
    /* degrees */
    norm("it", "21 °C", "ventuno gradi Celsius");
    norm("it", "1 °C", "un grado Celsius");
    /* symbols */
    norm("it", "n. 5", "numero cinque");
    norm("it", "#5", "numero cinque");
    norm("it", "A & B", "A e B");
    norm("it", "10-20", "dieci a venti");
    norm("it", "info@lordos.org", "info chiocciola lordos punto org");
    /* abbreviations */
    norm("it", "il Sig. Rossi e la Sig.ra Bianchi", "il signor Rossi e la signora Bianchi");
    norm("it", "Dott. Verdi", "dottor Verdi");
    norm("it", "mele, pere ecc. Poi", "mele, pere eccetera. Poi");
    norm("it", "a pag. 5", "a pagina cinque");
}

static void normalize_nl(void) {
    norm("nl", "Ik ben 88 jaar oud en ben geboren in 1987. Ik kocht 3 boeken voor 25 euro. #LordOS is geweldig.",
         "Ik ben achtentachtig jaar oud en ben geboren in negentienhonderdzevenentachtig. Ik kocht drie boeken voor "
         "vijfentwintig euro. LordOS is geweldig.");
    /* cardinals */
    norm("nl", "0", "nul");
    norm("nl", "1", "één");
    norm("nl", "13", "dertien");
    norm("nl", "21", "eenentwintig");
    norm("nl", "22", "tweeëntwintig");
    norm("nl", "23", "drieëntwintig");
    norm("nl", "80", "tachtig");
    norm("nl", "100", "honderd");
    norm("nl", "101", "honderdeen");
    norm("nl", "1000", "duizend");
    norm("nl", "2026", "tweeduizend zesentwintig");
    norm("nl", "1.200.000", "één miljoen tweehonderdduizend");
    norm("nl", "2.000.000", "twee miljoen");
    norm("nl", "1.000.000.000", "één miljard");
    norm("nl", "-5", "min vijf");
    norm("nl", "in 1990", "in negentienhonderdnegentig");
    /* decimals, percent */
    norm("nl", "3,5", "drie komma vijf");
    norm("nl", "25%", "vijfentwintig procent");
    /* ordinals */
    norm("nl", "de 1e", "de eerste");
    norm("nl", "de 2de", "de tweede");
    norm("nl", "de 3e", "de derde");
    norm("nl", "de 8ste", "de achtste");
    norm("nl", "de 19e", "de negentiende");
    norm("nl", "de 21ste", "de eenentwintigste");
    norm("nl", "de 100ste", "de honderdste");
    /* times */
    norm("nl", "14:30", "veertien uur dertig");
    norm("nl", "14:00 uur", "veertien uur");
    norm("nl", "1:02:03", "één uur, twee minuten en drie seconden");
    /* dates */
    norm("nl", "15-11-2026", "vijftien november tweeduizend zesentwintig");
    norm("nl", "01/05/2026", "één mei tweeduizend zesentwintig");
    /* money */
    norm("nl", "25 €", "vijfentwintig euro");
    norm("nl", "25,50 €", "vijfentwintig euro vijftig");
    norm("nl", "1.000.000 €", "één miljoen euro");
    norm("nl", "$10", "tien dollar");
    /* degrees */
    norm("nl", "21 °C", "eenentwintig graden Celsius");
    norm("nl", "1 °C", "één graad Celsius");
    /* symbols */
    norm("nl", "#5", "nummer vijf");
    norm("nl", "A & B", "A en B");
    norm("nl", "10-20", "tien tot twintig");
    /* abbreviations */
    norm("nl", "bijv. appels", "bijvoorbeeld appels");
    norm("nl", "d.w.z. morgen", "dat wil zeggen morgen");
    norm("nl", "appels, peren enz. Dan", "appels, peren enzovoort. Dan");
    norm("nl", "nr. 5", "nummer vijf");
    norm("nl", "dhr. Jansen en mevr. De Vries", "de heer Jansen en mevrouw De Vries");
    norm("nl", "1 t/m 5", "één tot en met vijf");
}

static void normalize_others(void) {
    /* English is left as it is: its models read digits */
    norm("en", "I am 88 years old, born in 1987. #LordOS costs $10 & 25%.", "I am 88 years old, born in 1987. #LordOS costs $10 & 25%.");
    norm("ru", "88", "88");
    norm(NULL, "88", "88");
    norm("en", "", "");
}

int main(void) {
    clean("colors out", "\x1b[1;32mlordbasex\x1b[0m@host:~$ ls", "lordbasex@host:~$ ls");
    clean("a window title out", "\x1b]0;title\x07hola", "hola");
    clean("spaces and tabs", "  uno \t  dos   tres ", "uno dos tres");
    clean("blank lines kept as one", "uno\n\n\n\ndos\ntres", "uno\n\ndos\ntres");
    clean("control characters out", "a\x01" "b\x7f" "c\rd", "abc d");
    clean("accents kept", "año, niño, acción", "año, niño, acción");
    clean("invalid UTF-8 out", "a\xff" "b\xc3" "c", "abc");
    clean("C1 controls out", "a\xc2\x9b" "b", "ab");
    split("sentences", "Hola. ¿Cómo estás? ¡Bien! Chau…", 200, "Hola.|¿Cómo estás?|¡Bien!|Chau…");
    split("a number is not a sentence end", "Versión 0.8.8.6 lista. Sí.", 200, "Versión 0.8.8.6 lista.|Sí.");
    split("blank lines", "Título\n\nPrimer párrafo", 200, "Título|Primer párrafo");
    split("a long sentence at a comma", "uno dos tres cuatro, cinco seis siete ocho nueve diez", 30, "uno dos tres cuatro,|cinco seis siete ocho nueve|diez");
    split("one long word on a character boundary", "ñññññññññññ", 9, "ññññ|ññññ|ñññ");
    split("nothing but space", "  \n ", 200, "");
    lang("Spanish", "Hola, ¿cómo estás? Esta es una prueba de la voz.", "en", "es");
    lang("English", "The quick brown fox jumps over the lazy dog and runs to the river.", "es", "en");
    lang("Portuguese", "Olá, tudo bem? Esta é uma prova da voz, não é muito difícil.", "es", "pt");
    lang("Portuguese by its letters", "A configuração do sistema está pronta para você.", "en", "pt");
    lang("Spanish over a Portuguese fallback", "El sistema está listo para que lo uses también.", "pt", "es");
    lang("English in capitals", "THE FILE IS NOT IN THE FOLDER", "es", "en");
    lang("too little to tell", "OK", "es", "es");
    lang("a command keeps the fallback", "make -C server test 2>&1 | tail", "pt", "pt");
    lang("empty", "", "en", "en");
    lang("a language without voices", "OK", "ru", "en");
    lang("terminal output in English", "Error: the file was not found in the folder and it can not be opened", "es", "en");
    lang("French", "Bonjour, je suis l'une des voix de LordOS. Je peux lire à voix haute tout texte que vous sélectionnez.", "en", "fr");
    lang("French with its elisions", "C'est l'été, qu'il fait beau et j'ai le temps.", "es", "fr");
    lang("French with curly apostrophes", "C\xe2\x80\x99est l\xe2\x80\x99été, qu\xe2\x80\x99il fait beau.", "es", "fr");
    lang("French over a Spanish fallback", "Le fichier est dans le dossier et il ne peut pas être ouvert.", "es", "fr");
    lang("German", "Hallo, ich bin eine der Stimmen von LordOS. Ich kann jeden Text, den du auswählst, laut vorlesen.", "en", "de");
    lang("German by its letters", "Größe und Maße", "en", "de");
    lang("Italian", "Ciao, sono una delle voci di LordOS. Posso leggere ad alta voce qualsiasi testo che selezioni.", "en", "it");
    lang("Italian over a Spanish fallback", "Il file non è nella cartella e non si può aprire.", "es", "it");
    lang("Dutch", "Hallo, ik ben een van de stemmen van LordOS. Ik kan elke tekst die je selecteert hardop voorlezen.", "en", "nl");
    lang("Dutch over a German fallback", "Het bestand is niet in de map en kan niet worden geopend.", "de", "nl");
    lang("too little to tell, a French fallback", "OK", "fr", "fr");
    lang("Spanish stays Spanish", "La inflación fue del 3,5% y el libro cuesta 25,50 €. La reunión es a las 14:30.", "en", "es");
    lang("Portuguese stays Portuguese", "A inflação foi de 3,5% e o livro custa R$ 25,50. A reunião é às 14:30.", "en", "pt");
    normalize_es();
    normalize_pt();
    normalize_fr();
    normalize_de();
    normalize_it();
    normalize_nl();
    normalize_others();
    normalize_region();
    printf("saytext_test: %d/%d\n", n - bad, n);
    return bad != 0;
}
