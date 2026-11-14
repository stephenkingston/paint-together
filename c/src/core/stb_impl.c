/*
 * Paint (C port) - the one place the vendored stb libraries are compiled
 *
 * third_party/stb: stb_image 2.30, stb_image_write 1.16, stb_truetype 1.26
 * (public domain / MIT). Only the decoders Paint offers are built.
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#define STBI_ONLY_PNM
#define STBI_NO_STDIO
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
