#include <boot/axboot.h>

void lyr_entry(struct aurix_parameters *params) {
  struct aurix_framebuffer framebuffer = params->framebuffer;

  volatile uint32_t *fb_ptr = (uint32_t *)framebuffer.addr;
  for (size_t y = 0; y < framebuffer.height; y++) {
    for (size_t x = 0; x < framebuffer.width; x++) {
      uint32_t nX = x * 255 / framebuffer.width;
      uint32_t nY = y * 255 / framebuffer.height;
      fb_ptr[y * (framebuffer.pitch / 4) + x] = (nY << 8) | nX;
    }
  }

  __asm__ volatile("cli");
  for (;;)
    __asm__ volatile("hlt");
}