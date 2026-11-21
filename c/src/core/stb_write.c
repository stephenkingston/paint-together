/*
 * Paint (C port) - stb_image_write (JPEG and BMP; PNG is img.c's), on its own
 * so that the Makefile can build it for speed: 2 KB more, and JPEGs save 15% faster
 */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
