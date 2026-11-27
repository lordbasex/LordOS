/* mcp.h — lordos-mcp's pure parts (cli/lordosmcp.c, docs/HANDS.md): the MCP tools' list, what a D-Bus error from
 * org.lordos.Hands1 tells the agent, a screenshot's pixels back to the desktop's logical points, base64, and the GIF a
 * recording is exported as. Tested by tests/hands_mcp_test.c. */
#ifndef LORD_HANDS_MCP_H
#define LORD_HANDS_MCP_H

#include <stddef.h>
#include <stdint.h>

#define MCP_PROTOCOL "2025-06-18"

/* tools/list's result: {"tools":[…]} with each tool's name, description and JSON Schema (static text) */
const char *mcp_tools_json(void);

/* the agent-facing sentence for a D-Bus error name (and its closed word): never the person's data */
const char *mcp_error_text(const char *error, const char *word, char *buf, size_t n);

/* a tool's answer when lord-server sent back nothing: "find" says no ref matched the query (echoed, the agent's own text:
 * at most 64 bytes, printable, cut at a character), "read_app_log" no line (matching its pattern), any other "done" */
const char *mcp_empty_text(const char *tool, const char *arg, char *buf, size_t n);

/* the last screenshot: what logical area it showed and its size; a point of it back on the desktop */
typedef struct { double x, y, w, h; int width, height, valid; } McpShot;
void mcp_shot_from_json(McpShot *s, const char *meta);
/* (px, py) in the screenshot → logical; without a screenshot, the point is already logical */
void mcp_map_point(const McpShot *s, double px, double py, double *lx, double *ly);

/* base64 of n bytes into out (malloc'd, NUL-terminated) */
char *mcp_base64(const uint8_t *p, size_t n);

/* ---- a recording (record start/stop/export) as an animated GIF ---- */
typedef struct { uint8_t *rgb; int w, h; } McpFrame;
/* frames (RGB, each at most 1024 × 1024, all fitted to the first's size) as a GIF89a, looping, delay_cs a frame;
 * malloc'd, its size in *len; NULL on failure */
uint8_t *mcp_gif(const McpFrame *f, int n, int delay_cs, size_t *len);

/* A recording's export: a bare file name (letters, digits, space, . _ -, 1 to 100 bytes, ending in .gif, not starting
 * with a dot), never a path. 1 ok. */
int mcp_export_name_ok(const char *name);
/* computer screenshot/zoom's save: the PNG's file name for what the agent gave ("login screen" or "login screen.png" →
 * "login screen.png"; the same rules as a recording's name, ending in .png). 0 ok, -1 not a bare name. */
int mcp_shot_save_name(const char *save, char *out, size_t n);
/* (a .gif recording or a .png screenshot) writes d as a NEW file name in <home>/Pictures/LordOS QA (each folder made if missing and opened O_DIRECTORY |
 * O_NOFOLLOW, the file O_CREAT | O_EXCL | O_NOFOLLOW, 0600): the only place lordos-mcp writes. 0 with its path in out,
 * -1 (errno) otherwise. */
int mcp_write_export(const char *home, const char *name, const uint8_t *d, size_t n, char *out, size_t on);
#define MCP_EXPORT_DIR "Pictures/LordOS QA"

/* MCP's JSON text helpers: a string quoted for JSON (malloc'd) */
char *mcp_quote(const char *s);

#endif
