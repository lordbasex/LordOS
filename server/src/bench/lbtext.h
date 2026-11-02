/* lbtext.h — the fixed paragraphs of LordBench's shaping workload (lbui.c, lb_shape): one per script, each
 * with what makes that script's shaping work (Latin kerning and ligatures, Arabic joining with bidi and
 * embedded left-to-right text, Devanagari conjuncts and reordered vowel signs, Thai stacked marks, Han
 * with kana). The fonts in src/bench/fonts are subset to exactly these characters by
 * tools/gen-lbfonts.py, which reads this file: change a paragraph and run it again. Plain UTF-8 literals,
 * one per line, no escapes; they are data, not interface text (i18n-ok). */
#ifndef LORD_BENCH_LBTEXT_H
#define LORD_BENCH_LBTEXT_H

enum { LB_PARA_LATIN, LB_PARA_ARABIC, LB_PARA_DEVANAGARI, LB_PARA_THAI, LB_PARA_CJK, LB_PARA_COUNT };

static const char *const lb_paragraphs[LB_PARA_COUNT] = {
    "LordOS draws its own desktop: a compositor written in C, a React interface on its own engine, and text shaped with OpenType kerning, ligatures and marks. Waffles, office files, fjords and affine fluffy flights test fi, fl, ff and ffi; quotes, dashes and accents too: café, naïve, São Paulo, Ångström, Kraków, Dvořák, Ærø. AVATAR, Type, Wave, You. Version 0.8.6, 1920×1080 at 60 Hz, 2026-10-28.", /* i18n-ok: benchmark data */
    "يرسم LordOS سطح المكتب بنفسه: مُركِّب نوافذ مكتوب بلغة C وواجهة React على محرّكه الخاص. الإصدار 0.8.6 يقيس المعالج والذاكرة والرسوميات، ويعرض النتائج بالأرقام 1920×1080 و60 هرتز. «النص العربي» يتّصل حرفاً بحرف، مع التشكيل والأرقام ١٢٣٤ (بين قوسين) وكلمات مثل لا وللّه والسلام.", /* i18n-ok: benchmark data */
    "LordOS अपना डेस्कटॉप स्वयं बनाता है: C में लिखा गया कंपोज़िटर और अपने इंजन पर चलने वाला इंटरफ़ेस। संयुक्ताक्षर जैसे क्ष, त्र, ज्ञ, श्र, द्ध, ह्म और र्क, मात्राएँ जैसे कि, की, कु, कृ, कें और कौं, सभी सही क्रम में दिखने चाहिए। संस्करण ०.८.६ प्रोसेसर, स्मृति और ग्राफ़िक्स को मापता है।", /* i18n-ok: benchmark data */
    "LordOS วาดเดสก์ท็อปของตัวเอง ด้วยคอมโพสิเตอร์ที่เขียนด้วยภาษา C และอินเทอร์เฟซบนเอนจินของตัวเอง ข้อความภาษาไทยมีวรรณยุกต์และสระซ้อนกัน เช่น ที่นี่ น้ำ ผู้ใหญ่ กำลัง ปี่ ญี่ปุ่น ฐาน ฎีกา และเลข ๐.๘.๖ ซึ่งต้องวางตำแหน่งให้ถูกต้อง", /* i18n-ok: benchmark data */
    "LordOS 自己绘制桌面：用 C 语言编写的合成器，以及运行在自有引擎上的界面。版本 0.8.6 会测量处理器、内存和图形性能，并以数字显示结果。日本語のテキスト：ひらがな、カタカナ、漢字が混ざった文章も正しく表示されます。「括弧」と句読点。", /* i18n-ok: benchmark data */
};

#endif
