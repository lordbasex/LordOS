/* stbzlib.c — stb_image's zlib decoder for lord-themec's .zip reader (zipread.c); the image decoders are compiled out
 * except PNG, which the decoder belongs to. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
