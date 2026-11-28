/* writing.h — "✨ AI" on selected text (docs/AI.md "Writing", docs/SPELLING.md): the fixed request for each action and
 * the engine's streamed answer, read strictly. Pure (tests/llm_engine_test.c); engine.c sends and reads.
 *
 * Only four actions exist, each with its texts fixed here: correct spelling and grammar, improve the writing, make
 * it shorter, translate into one language of a closed list. A short system text says the text is data; each user
 * message holds the task, then the text between two markers with a random nonce; one or two fixed worked examples go
 * first as earlier turns (a small model copied the text back without them, ws164). The answer is streamed (OpenAI's
 * server-sent events, as llama-server sends them); what comes back is only the model's text, at most LLM_WRITE_OUT_MAX
 * bytes, and the shell treats it as plain text (sdk/writingmodel.js cleanOutput). */
#ifndef LORD_LLM_WRITING_H
#define LORD_LLM_WRITING_H

#include <stddef.h>

#define LLM_WRITE_MAX       16000          /* the selection, bytes (the shell caps it at 4,000 characters) */
#define LLM_WRITE_OUT_MAX   16384          /* the model's text kept */
#define LLM_WRITE_TOKENS    1536

enum { WR_FIX = 1, WR_IMPROVE, WR_SHORTER, WR_TRANSLATE };

/* "fix", "improve", "shorter", "translate" → WR_*, else 0 */
int wr_action(const char *name);
/* A translation's target: a LordOS interface language ("es", "pt", "zh-Hans"…); its English name, or NULL. */
const char *wr_language(const char *code);
/* The request's JSON body (malloc'd) for an action, its target (translate only) and the text, with this nonce
 * (16 hex digits); NULL for a bad action, target or text (empty, too long, not UTF-8, a NUL). */
char *wr_request(int action, const char *target, const char *text, const char *nonce);

/* The HTTP answer, fed as it arrives: the head (status 200 only), then the body, chunked or not, as server-sent events
 * ("data: {…choices[0].delta.content…}", "data: [DONE]"). */
typedef struct {
    int phase;                  /* 0 the head, 1 a chunk's size line, 2 its data, 3 the CRLF after it, 4 the body as is */
    int chunked, done;
    size_t chunk_left;
    char head[8192];
    size_t hn;
    char line[65536];
    size_t ln;
    char text[LLM_WRITE_OUT_MAX + 1];
    size_t tn;
    int truncated;              /* the model wrote more than LLM_WRITE_OUT_MAX: the rest is dropped */
} WrStream;
void wr_stream_init(WrStream *s);
/* 0 more to come, 1 the answer is complete ("data: [DONE]"), -1 malformed (another status, a bad chunk, a line too
 * long, an event that isn't JSON). */
int wr_stream_feed(WrStream *s, const char *p, size_t n);

#endif
