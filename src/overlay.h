#pragma once
#include <cstdint>
// px: BGRA (0xAARRGGBB) framebuffer w*h. Draws text with a solid background box. Charset: 0-9 F P S C U G V K W % . - space
void OverlayDrawText(uint32_t* px, int w, int h, int x, int y, int scale, const char* text, uint32_t fg, uint32_t bg);
