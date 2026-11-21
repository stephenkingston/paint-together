/*
 * Paint (C port) - stb_truetype, on its own so that the Makefile can build it for speed
 * where that pays: built for size by gcc, new UI text sizes and the text tool drew 20-40% slower
 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
