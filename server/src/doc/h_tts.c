/* h_tts.c — lord-tts: text to speech with Pocket TTS (Kyutai, CC-BY-4.0) through audio.cpp (docs/VOICE.md).
 *
 * lord-tts is lord-doc's helper loop (helper.c, built with LORD_HELPER_TTS) with only this part: the only binary
 * that links libaudiocpp and ggml (server/mk/tts.mk), so neither lord-server nor any other helper does. Its
 * sandbox reads /usr (the libraries, the Vulkan drivers) and the model folder it was given as an fd; with the GPU
 * it also opens the render node, with DRM ioctls only, and writes Mesa's shader cache in a folder it was given.
 * It has no network and never opens a path it wasn't given.
 *
 * DQ_TTS: {"slot","file","text","backend","threads","voice_id","seed","language"} + optionally a WAV fd (the voice to
 * clone, up to 30 s) → DR_BLOB: a WAV (16-bit mono) in a sealed memfd; a sentence with hiss is made again (hiss.c).
 * The model ("file", relative to the read-only folder in "slot") is loaded by the first request and kept; a request
 * for another file, backend or language is refused. "file" is a GGUF (the standard models), or, with "language" (one
 * of the HD models: HD_LANGUAGES), a folder with Kyutai's model.safetensors, tokenizer.model and embeddings/, read
 * with pocket_tts_dir.json (audio.cpp's description of the model, reading that folder), next to libaudiocpp. */
#include "helper.h"
#include "../tts/hiss.h"

#include <audiocpp.h>

#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEXT_MAX 4000                  /* one request is a sentence or a few; say splits longer text */
#define VOICE_MAX_SECONDS 30
#define HISS_TRIES 3                   /* a noisy sentence is made again with another seed, at most twice */
#define WAV_MAX (64u << 20)

static audiocpp_registry *reg;
static audiocpp_model *model;
static audiocpp_session *sess;
static char loaded[300], loaded_backend[8], loaded_language[24];

/* Kyutai's 24-layer models ("HD"), named as audio.cpp knows them */
static const char *const HD_LANGUAGES[] = { "spanish_24l", "portuguese_24l", "english_2026-09_24l" };

static int valid_language(const char *s) {
    for (size_t i = 0; i < sizeof HD_LANGUAGES / sizeof *HD_LANGUAGES; i++) if (!strcmp(s, HD_LANGUAGES[i])) return 1;
    return 0;
}

/* What Kyutai's own code does to the text before the model (pocket_tts/config/<language>.yaml, replace_characters)
 * and audio.cpp doesn't: quotes, brackets and, in Spanish, ¡ ¿ are dropped, curly apostrophes become ', a dash a
 * comma. Without it the Spanish model stumbled over ¿…? and «…». The language is the model file's (es/, pt/, en/). */
static void kyutai_text(const char *in, char *out, size_t cap, char lang) {
    static const char *const drop_all[] = { "\"", "\xe2\x80\x9c", "\xe2\x80\x9d", "\xe2\x80\x9e", "\xc2\xab", "\xc2\xbb", "(", ")", "[", "]" };
    static const char *const drop_es[] = { "\xc2\xa1", "\xc2\xbf" };                  /* ¡ ¿ */
    size_t o = 0;
    for (const char *p = in; *p && o + 4 < cap;) {
        size_t skip = 0;
        if (lang != 'e')
            for (size_t i = 0; !skip && i < sizeof drop_all / sizeof *drop_all; i++)
                if (!strncmp(p, drop_all[i], strlen(drop_all[i]))) skip = strlen(drop_all[i]);
        if (lang == 's')
            for (size_t i = 0; !skip && i < 2; i++) if (!strncmp(p, drop_es[i], 2)) skip = 2;
        if (skip) { p += skip; continue; }
        if (!strncmp(p, "\xe2\x80\x99", 3) || !strncmp(p, "\xe2\x80\x98", 3)) { out[o++] = '\''; p += 3; continue; }   /* ’ ‘ */
        if (lang != 'e' && !strncmp(p, "\xe2\x80\x93", 3)) { out[o++] = ','; p += 3; continue; }                     /* – */
        out[o++] = *p++;
    }
    out[o] = 0;
}

/* pocket_tts_dir.json next to libaudiocpp: <this binary's folder>/../lib/tts installed, ./lib/tts in a build tree */
static int spec_path(char *out, size_t n) {
    char self[PATH_MAX];
    ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1);
    if (sl <= 0) return -1;
    self[sl] = 0;
    char *slash = strrchr(self, '/');
    if (!slash) return -1;
    *slash = 0;
    static const char *const rel[] = { "/../lib/tts/pocket_tts_dir.json", "/lib/tts/pocket_tts_dir.json" };
    for (int i = 0; i < 2; i++) {
        if ((size_t)snprintf(out, n, "%s%s", self, rel[i]) < n && !access(out, R_OK)) return 0;
    }
    return -1;
}

/* a file name under the model folder: letters, digits, "._-/", no "..", not absolute */
static int valid_file(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > 200 || s[0] == '/' || strstr(s, "..")) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || strchr("._-/", s[i]))) return 0;
    return 1;
}

static int valid_voice_id(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > 40) return 0;
    for (size_t i = 0; i < n; i++) if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) return 0;
    return 1;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* A voice to clone: RIFF/WAVE, PCM 16-bit or float 32-bit, 1 or 2 channels, 8–48 kHz, at most 30 s. Mono floats
 * (malloc) or NULL; every size is checked against the file's. */
static float *read_voice(int fd, size_t *frames, int *rate) {
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 44 || (uint64_t)st.st_size > WAV_MAX) return NULL;
    size_t len = (size_t)st.st_size;
    const uint8_t *p = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) return NULL;
    float *out = NULL;
    int fmt = 0, ch = 0, bits = 0;
    uint32_t sr = 0;
    const uint8_t *data = NULL;
    size_t dlen = 0;
    if (!memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WAVE", 4)) {
        for (size_t off = 12; off + 8 <= len;) {
            uint32_t sz = le32(p + off + 4);
            size_t body = off + 8;
            if (sz > len - body) sz = (uint32_t)(len - body);
            if (!memcmp(p + off, "fmt ", 4) && sz >= 16) {
                fmt = le16(p + body); ch = le16(p + body + 2); sr = le32(p + body + 4); bits = le16(p + body + 14);
                if (fmt == 0xFFFE && sz >= 26) fmt = le16(p + body + 24);          /* WAVE_FORMAT_EXTENSIBLE: the sub-format */
            } else if (!memcmp(p + off, "data", 4)) { data = p + body; dlen = sz; break; }
            off = body + sz + (sz & 1);
        }
    }
    int ok = data && (ch == 1 || ch == 2) && sr >= 8000 && sr <= 48000 && ((fmt == 1 && bits == 16) || (fmt == 3 && bits == 32));
    size_t bpf = ok ? (size_t)ch * (bits / 8) : 0;
    size_t n = ok ? dlen / bpf : 0;
    if (n > (size_t)sr * VOICE_MAX_SECONDS) n = (size_t)sr * VOICE_MAX_SECONDS;
    if (ok && n >= sr / 2 && (out = malloc(n * sizeof *out))) {
        for (size_t i = 0; i < n; i++) {
            float v = 0;
            for (int c = 0; c < ch; c++) {
                const uint8_t *s = data + i * bpf + (size_t)c * (bits / 8);
                if (fmt == 1) v += (int16_t)le16(s) / 32768.0f;
                else { uint32_t u = le32(s); float f; memcpy(&f, &u, 4); v += isfinite(f) ? f : 0; }
            }
            out[i] = v / ch;
        }
        *frames = n;
        *rate = (int)sr;
    }
    munmap((void *)p, len);
    return out;
}

/* the model, once: /proc/self/fd/<slot>/<file>, read through the folder fd the sandbox allows */
static const char *load(int slot_fd, const char *file, const char *backend, int threads, const char *language) {
    if (sess) return strcmp(file, loaded) || strcmp(backend, loaded_backend) || strcmp(language ? language : "", loaded_language)
        ? "another-model" : NULL;
    char path[300], spec[PATH_MAX];
    snprintf(path, sizeof path, "/proc/self/fd/%d/%s", slot_fd, file);
    if (!reg && audiocpp_registry_create(NULL, &reg) != AUDIOCPP_OK) return "engine";
    audiocpp_model_config mc = { .family_hint = "pocket_tts" };
    audiocpp_options *lo = NULL;
    if (language) {
        if (spec_path(spec, sizeof spec)) return "engine";
        mc.model_spec_override = spec;
        if (!(lo = audiocpp_options_create()) || audiocpp_options_set(lo, "language", language) != AUDIOCPP_OK) {
            if (lo) audiocpp_options_free(lo);
            return "engine";
        }
    }
    audiocpp_status st = audiocpp_model_load(reg, path, &mc, lo, &model);
    if (lo) audiocpp_options_free(lo);
    if (st != AUDIOCPP_OK) return "model";
    audiocpp_backend_config bc = { .backend = backend, .device = 0, .threads = threads };
    const char *task = audiocpp_model_supports(model, "clon", "offline") ? "clon" : "tts";
    if (audiocpp_session_create(model, task, "offline", &bc, NULL, &sess) != AUDIOCPP_OK) {
        audiocpp_model_free(model);
        model = NULL;
        return !strcmp(backend, "cpu") ? "session" : "gpu";
    }
    snprintf(loaded, sizeof loaded, "%s", file);
    snprintf(loaded_backend, sizeof loaded_backend, "%s", backend);
    snprintf(loaded_language, sizeof loaded_language, "%s", language ? language : "");
    return NULL;
}

/* float samples → a 16-bit mono WAV in a sealed memfd */
static int wav_memfd(const float *s, size_t frames, int rate, int ch, size_t *size) {
    size_t n = frames, bytes = n * 2;
    if (bytes > 0x7fffffffu - 44) return -1;
    int fd = memfd_create("lordos-tts", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, (off_t)(44 + bytes))) { if (fd >= 0) close(fd); return -1; }
    uint8_t *p = mmap(NULL, 44 + bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); return -1; }
    uint32_t r = (uint32_t)rate, b = (uint32_t)bytes;
    uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
                      0,0,0,0, 0,0,0,0, 2,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    uint32_t riff = 36 + b, br = r * 2;
    memcpy(h + 4, &riff, 4); memcpy(h + 24, &r, 4); memcpy(h + 28, &br, 4); memcpy(h + 40, &b, 4);
    memcpy(p, h, 44);
    int16_t *o = (int16_t *)(p + 44);
    for (size_t i = 0; i < n; i++) {
        float v = 0;
        for (int c = 0; c < ch; c++) v += s[i * (size_t)ch + (size_t)c];
        v /= ch;
        v = !isfinite(v) ? 0 : v > 1 ? 1 : v < -1 ? -1 : v;
        o[i] = (int16_t)lrintf(v * 32767.0f);
    }
    munmap(p, 44 + bytes);
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
    *size = 44 + bytes;
    return fd;
}

int tts_handle(const DocMsg *m, const JVal *args) {
    if (m->h.op != DQ_TTS) return 0;
    const char *text = json_str(args, "text"), *file = json_str(args, "file"), *backend = json_str(args, "backend");
    const char *voice_id = json_str(args, "voice_id"), *language = json_str(args, "language");
    int slot = slot_fd(args, "slot");
    int threads = (int)json_num(args, "threads", 4);
    if (!backend) backend = "cpu";
    if (!text || !*text || strlen(text) > TEXT_MAX || slot < 0 || !valid_file(file) || threads < 1 || threads > 64
        || (strcmp(backend, "cpu") && strcmp(backend, "vulkan")) || (voice_id && !valid_voice_id(voice_id))
        || (language && !valid_language(language))) {
        reply_err(m->h.id, "bad-request", "");
        return 1;
    }
    const char *err = load(slot, file, backend, threads, language);
    if (err) { reply_err(m->h.id, err, audiocpp_last_error() ? audiocpp_last_error() : ""); return 1; }
    audiocpp_request *rq = audiocpp_request_create();
    float *voice = NULL;
    if (!rq) { reply_err(m->h.id, "memory", ""); return 1; }
    /* the model's language from its file: es/…, pt/…, en/… (say.c's layout); s, p or e */
    char lang = !strncmp(file, "es/", 3) ? 's' : !strncmp(file, "pt/", 3) ? 'p' : 'e';
    char clean[TEXT_MAX + 1];
    kyutai_text(text, clean, sizeof clean, lang);
    audiocpp_request_set_text(rq, clean, NULL);
    /* Kyutai's recommended temperature for every model (pocket_tts's default_temperature); audio.cpp defaults to 0.7
     * outside English, which made Spanish drag and stumble */
    audiocpp_request_set_option(rq, "temperature", "0.3");
    if (m->nfds >= 1) {
        size_t frames = 0;
        int rate = 0;
        if (!(voice = read_voice(m->fds[0], &frames, &rate))) { audiocpp_request_free(rq); reply_err(m->h.id, "bad-voice", ""); return 1; }
        audiocpp_request_set_voice_audio(rq, voice, frames, rate, 1);
    } else if (voice_id) audiocpp_request_set_voice_id(rq, voice_id);
    /* now and then a sentence comes out with a bed of hiss under the voice (it depends on the sentence and the seed):
     * it is made again with another seed, up to HISS_TRIES times, and the cleanest is kept (hiss.c). A try the model
     * ended early never wins for being cleaner; when every try is cut short, the longest is kept. */
    double seed = json_num(args, "seed", 7);
    unsigned base = seed >= 0 && seed <= 4294967295.0 ? (unsigned)seed : 7;
    size_t syllables = tts_syllables(text);
    struct { audiocpp_result *r; const float *s; size_t frames; int rate, ch; double hiss; } t[HISS_TRIES];
    int n = 0, pick = -1;
    for (int try = 0; try < HISS_TRIES; try++) {
        char sbuf[16];
        snprintf(sbuf, sizeof sbuf, "%u", base + (unsigned)try * 1009u);
        audiocpp_request_set_option(rq, "seed", sbuf);
        audiocpp_result *r = NULL;
        const float *rs = NULL;
        size_t rf = 0;
        int rr = 0, rc = 0;
        if (audiocpp_session_run(sess, rq, &r) != AUDIOCPP_OK || !r || audiocpp_result_audio(r, &rs, &rf, &rr, &rc) != AUDIOCPP_OK
            || rc < 1 || rr < 1) {
            if (r) audiocpp_result_free(r);
            if (n) break;                           /* keep the ones already made */
            audiocpp_request_free(rq);
            free(voice);
            reply_err(m->h.id, "synthesis", audiocpp_last_error() ? audiocpp_last_error() : "");
            return 1;
        }
        t[n].r = r; t[n].s = rs; t[n].frames = rf; t[n].rate = rr; t[n].ch = rc; t[n].hiss = tts_hiss(rs, rf, rr, rc);
        n++;
        size_t longest = 0;
        for (int i = 0; i < n; i++) if (t[i].frames > longest) longest = t[i].frames;
        pick = -1;
        for (int i = 0; i < n; i++)
            if (!tts_cut_short(t[i].frames, t[i].rate, syllables, longest) && (pick < 0 || t[i].hiss < t[pick].hiss)) pick = i;
        if (pick >= 0 && t[pick].hiss <= TTS_HISS_MAX) break;
    }
    if (pick < 0)                                   /* all cut short: the longest */
        for (int i = 0; i < n; i++) if (pick < 0 || t[i].frames > t[pick].frames) pick = i;
    for (int i = 0; i < n; i++) if (i != pick) audiocpp_result_free(t[i].r);
    audiocpp_result *res = t[pick].r;
    const float *s = t[pick].s;
    size_t frames = t[pick].frames, size = 0;
    int rate = t[pick].rate, ch = t[pick].ch;
    audiocpp_request_free(rq);
    free(voice);
    int fd = wav_memfd(s, frames, rate, ch, &size);
    audiocpp_result_free(res);
    if (fd < 0) { reply_err(m->h.id, "memory", ""); return 1; }
    reply_blob(m->h.id, fd, size);
    return 1;
}
