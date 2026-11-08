/* say.c — lord-say, LordOS's `say` command (docs/VOICE.md): text to speech.
 *
 *   say "Hola, ¿cómo estás?"            say -l en -v alba "Hello"          echo texto | say
 *   say -f notas.txt -o notas.wav       say -b gpu "…"                      say --voices
 *   say -q hd -v juergen "…"             say --preview es lola
 *
 * It holds no speech code: it starts lord-tts (the sandboxed helper next to it, src/doc/h_tts.c) on a channel at
 * fd 3, gives it the models folder (read only) and, for the GPU, Mesa's shader cache folder, and sends it the text
 * a sentence at a time (saytext.c). Each sentence comes back as a WAV in a sealed memfd and is played with
 * PipeWire's pw-cat (or written with -o) while the next one is synthesized, so speech starts after the first.
 *
 * Models, root being /usr/share/lordos/voice (the lordos-voice-* packages) or ~/.local/share/lordos/voice:
 *   standard  <root>/models/<language>/standard.gguf, its voices in <root>/models/<language>/embeddings/<voice>.safetensors
 *   hd        <root>/models/<language>/hd/ (Kyutai's model.safetensors, tokenizer.model and embeddings/<voice>.safetensors)
 * The quality is -q, else voice.json's "quality", else "auto": HD when it is installed. The voice is -v, else
 * voice.json's "voices" for the language ({"es": "juergen"}), else its "voice", else the language's own (Lola,
 * Rafael, Alba); a voice that isn't installed for that model falls back to the language's own, then to any that is.
 * <root>/voices/<name>.wav is a voice to clone instead. Without -l, the language is the text's (say_language);
 * voice.json's "language", then $LANG's, when the text is too short to tell.
 *
 * --preview <language> <voice> plays the voice's recorded sample, <root>/previews/<language>/<voice>.wav (the
 * lordos-voice-previews package), so a voice can be heard before it is downloaded.
 *
 * -o - writes the WAV to stdout (its sizes are set at the end, so it must be a file); lord-server saves this way.
 *
 * --levels (lord-server's, speak.c): fd 3 is a pipe to it; for each SAY_LEVEL_MS of audio played, SAY_BANDS bytes, how
 * loud each band is (levels.c), for the wave its player draws. Written without waiting: one it isn't reading is lost.
 *
 * While it speaks or saves, ~/.cache/lordos/say/<pid> says how: "lord-say 1 <vulkan|cpu> <hd|standard> <language>
 * <speak|save>" (Monitor → GPU shows it: lord-tts is sandboxed and its /proc isn't readable, src/monitor/gpustat.c). It
 * goes when lord-say ends; a note whose process is gone is removed by the next lord-say. Not lord-tts's folder: the
 * helper can't write it.
 *
 * Exit status: 0 spoken, 1 an error, 2 usage, 3 no model for the language, 4 no voice chosen, 5 lord-tts isn't
 * installed, 130 stopped by a signal. lord-server (speak.c) tells the person which one it was. */
#include "../doc/proto.h"
#include "../doc/wire.h"
#include "../sec/json.h"
#include "levels.h"
#include "saytext.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEXT_MAX (4u << 20)
#define PIECE_MAX 300
#define WAV_PREVIEW_MAX (8u << 20)          /* a recorded sample: about ten seconds */

static const char *const ROOTS_SYS = "/usr/share/lordos/voice";
static int sock = -1;
static pid_t helper = -1, player = -1;
static int player_fd = -1;
static int levels_fd = -1;                /* --levels: fd 3 */

enum { EXIT_NO_MODEL = 3, EXIT_NO_VOICE = 4, EXIT_NOT_INSTALLED = 5 };

static void die_as(int code, const char *msg, const char *detail) {
    fprintf(stderr, "say: %s%s%s\n", msg, detail && *detail ? ": " : "", detail ? detail : "");
    exit(code);
}
static void die(const char *msg, const char *detail) { die_as(1, msg, detail); }

static void usage(void) {
    fputs("usage: say [-l es|en|pt] [-v voice] [-q auto|standard|hd] [-b auto|cpu|gpu] [-o file.wav|-] [-f file|-] [text…]\n"
          "       say --voices\n"
          "       say --preview es|en|pt <voice>\n", stderr);
    exit(2);
}

static char *read_all(int fd, size_t *len) {
    size_t cap = 4096, n = 0;
    char *b = malloc(cap + 1);
    for (;;) {
        if (!b) die("out of memory", NULL);
        if (n == cap) { if (cap >= TEXT_MAX) die("text too long", "4 MB at most"); cap *= 2; b = realloc(b, cap + 1); continue; }
        ssize_t r = read(fd, b + n, cap - n);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) die("can't read the text", strerror(errno));
        if (!r) break;
        n += (size_t)r;
    }
    b[n] = 0;
    *len = n;
    return b;
}

/* ~/.config/lordos/voice.json: the defaults Settings → AI → Voice keeps */
static char conf_lang[8], conf_voice[64], conf_backend[8], conf_quality[12], conf_voices[3][64];
static const char *const LANGS[3] = { "es", "en", "pt" };
static void read_settings(const char *home) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/.config/lordos/voice.json", home);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    size_t len;
    char *t = read_all(fd, &len);
    close(fd);
    JDoc d = { 0 };
    if (!json_parse(t, len, &d, NULL, 0) && d.root) {
        const char *v;
        if ((v = json_str(d.root, "language"))) snprintf(conf_lang, sizeof conf_lang, "%s", v);
        if ((v = json_str(d.root, "voice"))) snprintf(conf_voice, sizeof conf_voice, "%s", v);
        if ((v = json_str(d.root, "backend"))) snprintf(conf_backend, sizeof conf_backend, "%s", v);
        if ((v = json_str(d.root, "quality"))) snprintf(conf_quality, sizeof conf_quality, "%s", v);
        const JVal *vs = json_get(d.root, "voices");
        for (int i = 0; vs && i < 3; i++)
            if ((v = json_str(vs, LANGS[i]))) snprintf(conf_voices[i], sizeof conf_voices[i], "%s", v);
    }
    json_free(&d);
    free(t);
}

static int simple_name(const char *s, size_t max) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > max) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-')) return 0;
    return 1;
}

static int is_file(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }

/* a language's HD model as audio.cpp names it (h_tts.c's HD_LANGUAGES), and its own voice */
static const char *hd_language(const char *lang) {
    return !strcmp(lang, "es") ? "spanish_24l" : !strcmp(lang, "pt") ? "portuguese_24l" : "english_2026-09_24l";
}
static const char *own_voice(const char *lang) { return !strcmp(lang, "es") ? "lola" : !strcmp(lang, "pt") ? "rafael" : "alba"; }

/* <root>/models/<lang>/<model dir>embeddings/<voice>.safetensors: the voice is installed for that model */
static int has_voice(const char *root, const char *lang, int hd, const char *voice) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/models/%s/%sembeddings/%s.safetensors", root, lang, hd ? "hd/" : "", voice);
    return is_file(p);
}

static int has_model(const char *root, const char *lang, int hd) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/models/%s/%s", root, lang, hd ? "hd/model.safetensors" : "standard.gguf");
    return is_file(p);
}

/* every installed voice, by language and quality; then the voices to clone */
static void list_voices(const char *user_root) {
    const char *roots[2] = { user_root, ROOTS_SYS };
    for (int l = 0; l < 3; l++)
        for (int hd = 1; hd >= 0; hd--)
            for (int r = 0; r < 2; r++) {
                if (!has_model(roots[r], LANGS[l], hd)) continue;
                char dir[PATH_MAX];
                snprintf(dir, sizeof dir, "%s/models/%s/%sembeddings", roots[r], LANGS[l], hd ? "hd/" : "");
                DIR *d = opendir(dir);
                for (struct dirent *e; d && (e = readdir(d));) {
                    size_t n = strlen(e->d_name);
                    if (n > 12 && !strcmp(e->d_name + n - 12, ".safetensors"))
                        printf("%s %s %.*s\n", LANGS[l], hd ? "hd" : "standard", (int)(n - 12), e->d_name);
                }
                if (d) closedir(d);
            }
    for (int r = 0; r < 2; r++) {
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s/voices", roots[r]);
        DIR *d = opendir(dir);
        for (struct dirent *e; d && (e = readdir(d));) {
            size_t n = strlen(e->d_name);
            if (n > 4 && !strcmp(e->d_name + n - 4, ".wav")) printf("clone %.*s\n", (int)(n - 4), e->d_name);
        }
        if (d) closedir(d);
    }
}

/* ---- the helper ---- */

static void start_helper(int models_fd, int cache_fd, int gpu, const char *home) {
    char self[PATH_MAX], path[PATH_MAX + 16];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) die("can't find myself", NULL);
    self[n] = 0;
    char *slash = strrchr(self, '/');
    if (!slash) die("can't find myself", NULL);
    *slash = 0;
    snprintf(path, sizeof path, "%s/lord-tts", self);
    if (!is_file(path)) die_as(EXIT_NOT_INSTALLED, "speech isn't installed", path);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) die("socketpair", strerror(errno));
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 3);
    /* its only writable folder, ~/.cache/lordos/voice: audio.cpp unpacks a model's files in tmp/, Mesa keeps compiled
     * shaders in gpu/ */
    char henv[PATH_MAX + 8], tenv[PATH_MAX + 32], cenv[PATH_MAX + 48], *envp[6];
    int ne = 0;
    snprintf(henv, sizeof henv, "HOME=%s", home);
    envp[ne++] = henv;
    envp[ne++] = "LANG=C.UTF-8";
    snprintf(tenv, sizeof tenv, "TMPDIR=%s/.cache/lordos/voice/tmp", home);
    envp[ne++] = tenv;
    if (gpu) { snprintf(cenv, sizeof cenv, "MESA_SHADER_CACHE_DIR=%s/.cache/lordos/voice/gpu", home); envp[ne++] = cenv; }
    envp[ne] = NULL;
    char *argv[] = { "lord-tts", NULL };
    int r = posix_spawn(&helper, path, &fa, NULL, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    close(sv[1]);
    if (r) die("can't start lord-tts", strerror(r));
    sock = sv[0];
    int fds[1] = { models_fd };
    doc_send(sock, DQ_INIT_FD, 0, "{\"role\":\"ro\"}", 13, fds, 1);
    if (cache_fd >= 0) { fds[0] = cache_fd; doc_send(sock, DQ_INIT_FD, 0, "{\"role\":\"rw\"}", 13, fds, 1); }
    const char *go = gpu ? "{\"mode\":\"tts\",\"gpu\":true}" : "{\"mode\":\"tts\"}";
    doc_send(sock, DQ_INIT_GO, 0, go, strlen(go), NULL, 0);
    DocMsg m;
    if (doc_recv(sock, &m) || m.h.op != DR_DONE) die("lord-tts didn't start", NULL);
    doc_msg_free(&m);
}

static void stop_helper(void) {
    if (sock >= 0) { close(sock); sock = -1; }
    if (helper > 0) { kill(helper, SIGKILL); waitpid(helper, NULL, 0); helper = -1; }
}

/* one piece of text → a request (id = piece number) */
static void request(uint32_t id, const char *text, size_t len, const char *file, const char *backend, int threads,
                    const char *voice_id, int voice_fd, const char *language) {
    JBuf b;
    jb_init(&b);
    jb_raw(&b, "{\"slot\":0,\"file\":");
    jb_str(&b, file);
    jb_raw(&b, ",\"text\":");
    jb_strn(&b, text, len);
    jb_printf(&b, ",\"backend\":\"%s\",\"threads\":%d,\"seed\":7", backend, threads);
    if (voice_id) { jb_raw(&b, ",\"voice_id\":"); jb_str(&b, voice_id); }
    if (language) { jb_raw(&b, ",\"language\":"); jb_str(&b, language); }
    jb_raw(&b, "}");
    int fds[1] = { voice_fd };
    if (b.oom || doc_send(sock, DQ_TTS, id, b.p, b.n, voice_fd >= 0 ? fds : NULL, voice_fd >= 0 ? 1 : 0)) die("lord-tts stopped", NULL);
    jb_free(&b);
}

/* the reply: the WAV's 16-bit samples (mmap of the memfd, *pcm into it) or an error code */
typedef struct { uint8_t *map; size_t maplen; const int16_t *pcm; size_t n; int rate; char err[64]; char detail[200]; } Piece;

static void reply(Piece *p) {
    memset(p, 0, sizeof *p);
    DocMsg m;
    if (doc_recv(sock, &m)) { snprintf(p->err, sizeof p->err, "crash"); return; }
    if (m.h.op == DR_ERR) {
        JDoc d = { 0 };
        if (m.json && !json_parse(m.json, m.len, &d, NULL, 0)) {
            const char *e = json_str(d.root, "error"), *t = json_str(d.root, "detail");
            snprintf(p->err, sizeof p->err, "%s", e ? e : "error");
            snprintf(p->detail, sizeof p->detail, "%s", t ? t : "");
        }
        json_free(&d);
    } else if (m.h.op == DR_BLOB && m.nfds == 1) {
        struct stat st;
        if (!fstat(m.fds[0], &st) && st.st_size > 44 && (p->map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, m.fds[0], 0)) != MAP_FAILED) {
            p->maplen = (size_t)st.st_size;
            uint32_t rate, dlen;
            memcpy(&rate, p->map + 24, 4);
            memcpy(&dlen, p->map + 40, 4);
            if (!memcmp(p->map, "RIFF", 4) && dlen <= p->maplen - 44 && rate >= 8000 && rate <= 96000) {
                p->pcm = (const int16_t *)(p->map + 44);
                p->n = dlen / 2;
                p->rate = (int)rate;
            } else snprintf(p->err, sizeof p->err, "bad-audio");
        } else { p->map = NULL; snprintf(p->err, sizeof p->err, "bad-audio"); }
    } else snprintf(p->err, sizeof p->err, "protocol");
    doc_msg_free(&m);
}

static void piece_free(Piece *p) { if (p->map) munmap(p->map, p->maplen); p->map = NULL; }

static int write_all(int fd, const void *buf, size_t n) {
    const char *b = buf;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

/* the model starts every text with 0.3–0.9 s of near silence (audio.cpp's own CLI does too): it is cut from the
 * first sentence, keeping LEAD_KEEP_MS, so speech starts as soon as it is asked for */
#define LEAD_KEEP_MS 50
static size_t lead_silence(const int16_t *pcm, size_t n, int rate) {
    size_t hop = (size_t)rate / 100, at = 0;
    while (hop && at + hop <= n) {
        double e = 0;
        for (size_t i = at; i < at + hop; i++) e += (double)pcm[i] * pcm[i];
        if (sqrt(e / hop) > 0.01 * 32768) break;
        at += hop;
    }
    if (!hop || at + hop > n) return 0;            /* no speech found: nothing is cut */
    size_t keep = (size_t)rate * LEAD_KEEP_MS / 1000;
    return at > keep ? at - keep : 0;
}

/* plays a piece, SAY_LEVEL_MS at a time, each after its bands to --levels */
static int play(const int16_t *pcm, size_t n, int rate) {
    if (levels_fd < 0) return write_all(player_fd, pcm, n * 2);
    size_t step = (size_t)rate * SAY_LEVEL_MS / 1000;
    for (size_t at = 0; at < n; at += step) {
        size_t k = n - at < step ? n - at : step;
        uint8_t b[SAY_BANDS];
        say_levels(pcm + at, k, rate, b);
        if (write(levels_fd, b, sizeof b) < 0 && errno != EAGAIN && errno != EINTR) { close(levels_fd); levels_fd = -1; }
        if (write_all(player_fd, pcm + at, k * 2)) return -1;
    }
    return 0;
}

static void start_player(int rate) {
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC)) die("pipe", strerror(errno));
    char r[16];
    snprintf(r, sizeof r, "%d", rate);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pfd[0], 0);
    char *argv[] = { "pw-cat", "--playback", "--raw", "--format", "s16", "--rate", r, "--channels", "1", "--media-role", "Speech", "-", NULL };
    int e = posix_spawnp(&player, "pw-cat", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pfd[0]);
    if (e) die("can't play audio (pw-cat, from pipewire-bin)", strerror(e));
    player_fd = pfd[1];
}

/* --preview: a voice's recorded sample, <root>/previews/<lang>/<voice>.wav, a 16-bit mono WAV; played as speech is */
static int play_preview(const char *const roots[2], const char *lang, const char *voice) {
    int ok_lang = 0;
    for (int i = 0; i < 3; i++) ok_lang |= !strcmp(lang, LANGS[i]);
    if (!ok_lang || !simple_name(voice, 60)) die("a preview is a language (es, en, pt) and a voice name", NULL);
    int fd = -1;
    for (int r = 0; r < 2 && fd < 0; r++) {
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/previews/%s/%s.wav", roots[r], lang, voice);
        fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    }
    if (fd < 0) die_as(EXIT_NO_VOICE, "no recorded sample of that voice", voice);
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 44 || st.st_size > (off_t)WAV_PREVIEW_MAX) die("not a sample", voice);
    size_t len = (size_t)st.st_size;
    const uint8_t *w = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (w == MAP_FAILED) die("can't read", voice);
    /* RIFF/WAVE, its fmt chunk PCM 16-bit mono, then its data chunk; every size checked against the file */
    uint32_t rate = 0, v32;
    uint16_t fmt = 0, ch = 0, bits = 0;
    const int16_t *pcm = NULL;
    size_t n = 0;
    if (memcmp(w, "RIFF", 4) || memcmp(w + 8, "WAVE", 4)) die("not a WAV", voice);
    for (size_t at = 12; at + 8 <= len;) {
        memcpy(&v32, w + at + 4, 4);
        size_t sz = v32;
        if (sz > len - at - 8) die("a broken WAV", voice);
        if (!memcmp(w + at, "fmt ", 4) && sz >= 16) {
            memcpy(&fmt, w + at + 8, 2); memcpy(&ch, w + at + 10, 2); memcpy(&rate, w + at + 12, 4); memcpy(&bits, w + at + 22, 2);
        } else if (!memcmp(w + at, "data", 4)) { pcm = (const int16_t *)(w + at + 8); n = sz / 2; break; }
        at += 8 + sz + (sz & 1);
    }
    if (fmt != 1 || ch != 1 || bits != 16 || rate < 8000 || rate > 96000 || !pcm || !n) die("the sample isn't 16-bit mono PCM", voice);
    start_player((int)rate);
    int r = play(pcm, n, (int)rate);
    close(player_fd);
    int ps;
    while (waitpid(player, &ps, 0) < 0 && errno == EINTR) {}
    munmap((void *)w, len);
    return r ? 1 : 0;
}

/* ---- the status note (Monitor → GPU) ---- */

static char note_path[PATH_MAX];

static void note_remove(void) { if (note_path[0]) unlink(note_path); }

static void note_write(const char *home, int gpu, int hd, const char *lang, int save) {
    char dir[PATH_MAX], tmp[PATH_MAX + 8];
    snprintf(dir, sizeof dir, "%s/.cache/lordos/say", home);
    mkdir(dir, 0700);
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (dfd < 0 || fstat(dfd, &st) || st.st_uid != getuid()) { if (dfd >= 0) close(dfd); return; }
    DIR *d = fdopendir(dup(dfd));
    for (struct dirent *e; d && (e = readdir(d));) {          /* notes left by a lord-say that was killed */
        char *end;
        long pid = strtol(e->d_name, &end, 10);
        if (e->d_name[0] >= '1' && e->d_name[0] <= '9' && !*end && pid > 0 && pid != getpid() && kill((pid_t)pid, 0) && errno == ESRCH)
            unlinkat(dfd, e->d_name, 0);
    }
    if (d) closedir(d);
    close(dfd);
    if (!note_path[0]) atexit(note_remove);
    snprintf(note_path, sizeof note_path, "%s/%d", dir, (int)getpid());
    snprintf(tmp, sizeof tmp, "%s.tmp", note_path);
    FILE *f = fopen(tmp, "we");
    if (!f) return;
    fprintf(f, "lord-say 1 %s %s %s %s\n", gpu ? "vulkan" : "cpu", hd ? "hd" : "standard", lang, save ? "save" : "speak");
    if (fclose(f) || rename(tmp, note_path)) unlink(tmp);
}

static void on_signal(int sig) {
    (void)sig;
    if (note_path[0]) unlink(note_path);
    if (helper > 0) kill(helper, SIGKILL);
    if (player > 0) kill(player, SIGTERM);
    _exit(130);
}

int main(int argc, char **argv) {
    const char *lang = NULL, *voice = NULL, *quality = NULL, *backend = NULL, *out = NULL, *file = NULL;
    int voices = 0, nwords = 0;
    const char *preview_lang = NULL, *preview_voice = NULL;
    /* options anywhere, as `say "texto" -o texto.wav` reads naturally; after "--", only words */
    char **words = calloc((size_t)argc, sizeof *words);
    if (!words) die("out of memory", NULL);
    for (int ai = 1, only_words = 0; ai < argc; ai++) {
        const char *a = argv[ai];
        if (only_words || a[0] != '-' || !a[1]) { words[nwords++] = argv[ai]; continue; }
        if (!strcmp(a, "--")) { only_words = 1; continue; }
        if (!strcmp(a, "--voices")) { voices = 1; continue; }
        if (!strcmp(a, "--preview")) {
            if (ai + 2 >= argc) usage();
            preview_lang = argv[++ai];
            preview_voice = argv[++ai];
            continue;
        }
        if (!strcmp(a, "--levels")) {
            struct stat st;
            if (!fstat(3, &st) && S_ISFIFO(st.st_mode) && !fcntl(3, F_SETFD, FD_CLOEXEC) && !fcntl(3, F_SETFL, O_WRONLY | O_NONBLOCK)) levels_fd = 3;
            continue;
        }
        if (!strcmp(a, "-h") || !strcmp(a, "--help") || a[2] || ai + 1 >= argc) usage();
        const char *v = argv[++ai];
        switch (a[1]) {
        case 'l': lang = v; break;
        case 'v': voice = v; break;
        case 'q': quality = v; break;
        case 'b': backend = v; break;
        case 'o': out = v; break;
        case 'f': file = v; break;
        default: usage();
        }
    }
    const char *home = getenv("HOME");
    if (!home || home[0] != '/' || strlen(home) > 1024) die("HOME isn't set", NULL);
    char user_root[1100];
    snprintf(user_root, sizeof user_root, "%s/.local/share/lordos/voice", home);
    if (voices) { list_voices(user_root); return 0; }
    if (preview_lang) {
        const char *const proots[2] = { user_root, ROOTS_SYS };
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
        signal(SIGPIPE, SIG_IGN);
        return play_preview(proots, preview_lang, preview_voice);
    }
    read_settings(home);

    /* the text: the arguments, a file, or stdin */
    size_t len = 0;
    char *text;
    if (file || !nwords) {
        int fd = !file || !strcmp(file, "-") ? 0 : open(file, O_RDONLY | O_CLOEXEC);
        if (fd < 0) die("can't open", file);
        text = read_all(fd, &len);
        if (fd) close(fd);
    } else {
        for (int i = 0; i < nwords; i++) len += strlen(words[i]) + 1;
        if (len > TEXT_MAX) die("text too long", "4 MB at most");
        text = malloc(len + 1);
        if (!text) die("out of memory", NULL);
        len = 0;
        for (int i = 0; i < nwords; i++) { size_t l = strlen(words[i]); memcpy(text + len, words[i], l); len += l; text[len++] = ' '; }
        text[len] = 0;
    }
    len = say_clean(text, len);
    if (!len) return 0;

    /* language: -l, or the text's own (saytext.c), the settings' or $LANG's when it is too short to tell; quality, model */
    if (!lang) {
        const char *fallback = conf_lang[0] ? conf_lang : NULL;
        char envlang[3] = "";
        if (!fallback) { const char *l = getenv("LANG"); if (l && strlen(l) >= 2) { envlang[0] = l[0]; envlang[1] = l[1]; fallback = envlang; } }
        lang = say_language(text, len, fallback);
    }
    if (strcmp(lang, "es") && strcmp(lang, "en") && strcmp(lang, "pt")) lang = "en";    /* the languages LordOS has voices for */
    if (!quality) quality = conf_quality[0] ? conf_quality : "auto";
    if (strcmp(quality, "auto") && strcmp(quality, "standard") && strcmp(quality, "hd")) die("quality is auto, standard or hd", quality);
    const char *roots[2] = { user_root, ROOTS_SYS };
    int li = !strcmp(lang, "es") ? 0 : !strcmp(lang, "en") ? 1 : 2;

    /* the voice asked for: -v, the language's in voice.json, voice.json's own; a WAV to clone when there is one */
    if (!voice) voice = conf_voices[li][0] ? conf_voices[li] : conf_voice[0] ? conf_voice : NULL;
    if (voice && !simple_name(voice, 60)) die("a voice name is letters, digits, - and _", voice);
    int voice_fd = -1;
    char path[PATH_MAX + 128];
    for (int r = 0; voice && r < 2 && voice_fd < 0; r++) {
        snprintf(path, sizeof path, "%s/voices/%s.wav", roots[r], voice);
        voice_fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    }

    /* the model: HD first unless standard was asked for (auto prefers HD), then the other; in either root */
    const char *root = NULL;
    int hd = 0;
    for (int pass = 0; pass < 2 && !root; pass++) {
        int want_hd = !strcmp(quality, "standard") ? pass : !pass;
        for (int r = 0; r < 2 && !root; r++)
            if (has_model(roots[r], lang, want_hd)) { root = roots[r]; hd = want_hd; }
    }
    if (!root) die_as(EXIT_NO_MODEL, "no voice model for this language; download one in Settings → AI → Voice", lang);

    /* a voice that model carries: the one asked for, else the language's own, else any installed */
    const char *voice_id = NULL;
    static char any[64];
    if (voice_fd < 0) {
        if (voice && has_voice(root, lang, hd, voice)) voice_id = voice;
        else if (has_voice(root, lang, hd, own_voice(lang))) voice_id = own_voice(lang);
        else {
            char dir[PATH_MAX];
            snprintf(dir, sizeof dir, "%s/models/%s/%sembeddings", root, lang, hd ? "hd/" : "");
            DIR *d = opendir(dir);
            for (struct dirent *e; d && (e = readdir(d)) && !voice_id;) {
                size_t n = strlen(e->d_name);
                if (n > 12 && n - 12 < sizeof any && !strcmp(e->d_name + n - 12, ".safetensors")) {
                    snprintf(any, sizeof any, "%.*s", (int)(n - 12), e->d_name);
                    if (simple_name(any, 60)) voice_id = any;
                }
            }
            if (d) closedir(d);
        }
        if (!voice_id) die_as(EXIT_NO_VOICE, "no voice installed for this language; download one in Settings → AI → Voice", lang);
    }
    char model_file[64];
    snprintf(model_file, sizeof model_file, "%s/%s", lang, hd ? "hd" : "standard.gguf");
    const char *language = hd ? hd_language(lang) : NULL;
    snprintf(path, sizeof path, "%s/models", root);
    int models_fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (models_fd < 0) die("can't open", path);

    /* where it runs: the GPU (Vulkan) when there is a render node, unless told otherwise */
    if (!backend) backend = conf_backend[0] ? conf_backend : "auto";
    if (strcmp(backend, "auto") && strcmp(backend, "cpu") && strcmp(backend, "gpu")) die("backend is auto, cpu or gpu", backend);
    int gpu = !strcmp(backend, "gpu") || (!strcmp(backend, "auto") && !access("/dev/dri/renderD128", F_OK));
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int threads = ncpu > 1 ? (int)(ncpu / 2 > 8 ? 8 : ncpu / 2) : 1;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    char c[1200];
    static const char *const sub[] = { "/.cache", "/.cache/lordos", "/.cache/lordos/voice", "/.cache/lordos/voice/tmp", "/.cache/lordos/voice/gpu" };
    for (size_t i = 0; i < sizeof sub / sizeof *sub; i++) { snprintf(c, sizeof c, "%s%s", home, sub[i]); mkdir(c, 0700); }
    snprintf(c, sizeof c, "%s/.cache/lordos/voice", home);
    int cache_fd = open(c, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat cst;
    if (cache_fd < 0 || fstat(cache_fd, &cst) || cst.st_uid != getuid()) die("can't use", c);
    for (int attempt = 0; attempt < 2; attempt++) {
        start_helper(models_fd, cache_fd, gpu, home);
        /* the first piece tells whether the GPU works; "auto" falls back to the CPU */
        size_t skip, k = say_next(text, len, PIECE_MAX, &skip);
        size_t off = skip + k;
        request(1, text + skip, k, model_file, gpu ? "vulkan" : "cpu", threads, voice_id, voice_fd, language);
        Piece cur;
        reply(&cur);
        if (cur.err[0] && gpu && !strcmp(backend, "auto") && attempt == 0) {
            stop_helper();
            gpu = 0;
            continue;
        }
        if (cur.err[0]) {
            if (!strcmp(cur.err, "bad-voice")) die("the voice isn't a usable WAV (16-bit or float, 0.5–30 s)", NULL);
            fprintf(stderr, "say: speech failed: %s%s%s\n", cur.err, cur.detail[0] ? ": " : "", cur.detail);
            return 1;
        }
        note_write(home, gpu, hd, lang, out != NULL);
        FILE *wav = NULL;
        size_t total = 0;
        if (out) {
            wav = !strcmp(out, "-") ? fdopen(dup(1), "wb") : fopen(out, "wb");
            if (!wav) die("can't write", out);
            fwrite(cur.map, 1, 44, wav);                  /* the header; its sizes are fixed at the end */
        } else start_player(cur.rate);
        for (uint32_t id = 2;; id++) {
            size_t k2 = say_next(text + off, len - off, PIECE_MAX, &skip);
            if (k2) request(id, text + off + skip, k2, model_file, gpu ? "vulkan" : "cpu", threads, voice_id, voice_fd, language);
            off += skip + k2;
            /* while the next piece is synthesized, this one plays */
            if (id == 2) { size_t cut = lead_silence(cur.pcm, cur.n, cur.rate); cur.pcm += cut; cur.n -= cut; }
            if (wav) fwrite(cur.pcm, 2, cur.n, wav);
            else if (play(cur.pcm, cur.n, cur.rate)) break;
            total += cur.n * 2;
            piece_free(&cur);
            if (!k2) break;
            reply(&cur);
            if (cur.err[0]) { fprintf(stderr, "say: a sentence failed: %s\n", cur.err); if (!strcmp(cur.err, "crash")) break; cur.n = 0; }
        }
        stop_helper();
        if (wav) {
            uint32_t riff = 36 + (uint32_t)total, data = (uint32_t)total;
            fseek(wav, 4, SEEK_SET); fwrite(&riff, 4, 1, wav);
            fseek(wav, 40, SEEK_SET); fwrite(&data, 4, 1, wav);
            if (fclose(wav)) die("can't write", out);
        } else {
            close(player_fd);
            int st;
            while (waitpid(player, &st, 0) < 0 && errno == EINTR) {}
        }
        return 0;
    }
    return 1;
}
