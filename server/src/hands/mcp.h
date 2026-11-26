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

/* MCP's JSON text helpers: a string quoted for JSON (malloc'd) */
char *mcp_quote(const char *s);

#endif
