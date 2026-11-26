/* mcpimg.c — a screenshot's PNG back to RGB for lordos-mcp's recordings (record → GIF), with vendored stb_image (PNG
 * only). The PNG is lord-server's own (Hands1's Screenshot). */
#include "mcpimg.h"

#include <stdlib.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include "stb_image.h"
#pragma GCC diagnostic pop
#pragma GCC diagnostic ignored "-Wunused-function"      /* stb_image's other loaders, reported at the end of the file */

uint8_t *mcp_png_rgb(const uint8_t *png, size_t n, int *w, int *h) {
    int c = 0;
    if (!png || n < 8 || n > (64u << 20)) return NULL;
    uint8_t *p = stbi_load_from_memory(png, (int)n, w, h, &c, 3);
    if (p && (*w < 1 || *h < 1 || *w > 8192 || *h > 8192)) { stbi_image_free(p); return NULL; }
    return p;
}
void mcp_png_free(uint8_t *p) { stbi_image_free(p); }
