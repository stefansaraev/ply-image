/* ply-image.c - png file loader
 *
 * Copyright (C) 2006, 2007 Red Hat, Inc.
 * Copyright (C) 2003 University of Southern California
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Some implementation taken from the cairo library.
 *
 * Written by: Charlie Brej <cbrej@cs.man.ac.uk>
 *             Kristian Høgsberg <krh@redhat.com>
 *             Ray Strode <rstrode@redhat.com>
 *             Carl D. Worth <cworth@cworth.org>
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <png.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define MIN(a,b) ((a) <= (b)? (a) : (b))
#define MAX(a,b) ((a) >= (b)? (a) : (b))

typedef struct _fb fb_t;
typedef struct _fb_area fb_area_t;

struct _fb_area
{
  long x;
  long y;
  unsigned long width;
  unsigned long height;
};

struct _fb
{
  char* device_name;
  int device_fd;

  char* map_addr;
  size_t size;

  uint32_t* shadow_buf;

  uint32_t r_bit_pos;
  uint32_t g_bit_pos;
  uint32_t b_bit_pos;
  uint32_t a_bit_pos;

  uint32_t bits_for_r;
  uint32_t bits_for_g;
  uint32_t bits_for_b;
  uint32_t bits_for_a;

  unsigned int bpp;
  unsigned int row_stride;

  fb_area_t area;
  fb_area_t area_to_flush;

  void (*flush)(fb_t* buf);

  int pause_count;
};

fb_t* fb_new(const char* device_name)
{
  fb_t* buf;
  buf = calloc(1, sizeof(fb_t));

  if (device_name != NULL)
    buf->device_name = strdup(device_name);
  else
    buf->device_name = strdup("/dev/fb0");

  buf->map_addr = MAP_FAILED;
  buf->shadow_buf = NULL;
  buf->pause_count = 0;
  return buf;
}

bool fb_device_is_open(fb_t* buf)
{
  return buf->device_fd >= 0 && buf->map_addr != MAP_FAILED;
}

static void fb_close_device(fb_t* buf)
{
  if (buf->map_addr != MAP_FAILED)
  {
    munmap(buf->map_addr, buf->size);
    buf->map_addr = MAP_FAILED;
  }

  if (buf->device_fd >= 0)
  {
    close(buf->device_fd);
    buf->device_fd = -1;
  }
}

void fb_close(fb_t* buf)
{
  fb_close_device(buf);
  buf->bpp = 0;
  buf->area.x = 0;
  buf->area.y = 0;
  buf->area.width = 0;
  buf->area.height = 0;
}

void fb_free(fb_t* buf)
{
  if (fb_device_is_open(buf))
    fb_close(buf);

  free(buf->device_name);
  free(buf->shadow_buf);
  free(buf);
}

static bool fb_open_device(fb_t* buf)
{
  buf->device_fd = open(buf->device_name, O_RDWR);

  if (buf->device_fd < 0)
    return false;

  return true;
}

static bool fb_flush(fb_t* buf)
{
  if (buf->pause_count > 0)
    return true;

  (*buf->flush)(buf);
  buf->area_to_flush.x = buf->area.width - 1;
  buf->area_to_flush.y = buf->area.height - 1;
  buf->area_to_flush.width = 0;
  buf->area_to_flush.height = 0;
  return true;
}

void fb_get_size(fb_t* buf, fb_area_t* size)
{
  *size = buf->area;
}

static void fb_area_union(fb_area_t* area1, fb_area_t* area2, fb_area_t* result)
{
  unsigned long x1, y1, x2, y2;

  if (area1->width == 0)
  {
    *result = *area2;
    return;
  }

  if (area2->width == 0)
  {
    *result = *area1;
    return;
  }

  x1 = area1->x + area1->width;
  y1 = area1->y + area1->height;
  x2 = area2->x + area2->width;
  y2 = area2->y + area2->height;
  result->x = MIN(area1->x, area2->x);
  result->y = MIN(area1->y, area2->y);
  result->width = MAX(x1, x2) - result->x;
  result->height = MAX(y1, y2) - result->y;
}

static void fb_area_intersect(fb_area_t* area1, fb_area_t* area2, fb_area_t* result)
{
  long x1, y1, x2, y2;
  long width, height;

  if (area1->width == 0)
  {
    *result = *area1;
    return;
  }

  if (area2->width == 0)
  {
    *result = *area2;
    return;
  }

  x1 = area1->x + area1->width;
  y1 = area1->y + area1->height;
  x2 = area2->x + area2->width;
  y2 = area2->y + area2->height;
  result->x = MAX(area1->x, area2->x);
  result->y = MAX(area1->y, area2->y);
  width = MIN(x1, x2) - result->x;
  height = MIN(y1, y2) - result->y;

  if (width <= 0 || height <= 0)
  {
    result->width = 0;
    result->height = 0;
  }
  else
  {
    result->width = width;
    result->height = height;
  }
}

static void fb_add_area_to_flush_area(fb_t* buf, fb_area_t* area)
{
  fb_area_t cropped_area;
  fb_area_intersect(area, &buf->area, &cropped_area);

  if (cropped_area.width == 0 || cropped_area.height == 0)
    return;

  fb_area_union(&buf->area_to_flush, &cropped_area, &buf->area_to_flush);
}


bool fb_fill_with_argb32_data(fb_t* buf, fb_area_t* area, unsigned long x, unsigned long y, uint32_t* data)
{
  long row, column;

  if (area == NULL)
    area = &buf->area;

  for (row = y; row < y + area->height; row++)
  {
    for (column = x; column < x + area->width; column++)
    {
      buf->shadow_buf[(row - y) * buf->area.width - x + column] = data[area->width * row + column];
    }
  }

  fb_add_area_to_flush_area(buf, area);
  return fb_flush(buf);
}

typedef union
{
  uint32_t* as_pixels;
  png_byte* as_png_bytes;
  char* address;
} image_layout_t;

typedef struct _image
{
  char* filename;
  FILE* fp;

  image_layout_t layout;
  size_t size;

  long width;
  long height;
} image_t;

static bool image_open_file(image_t* image)
{
  image->fp = fopen(image->filename, "r");

  if (image->fp == NULL)
    return false;

  return true;
}

static void image_close_file(image_t* image)
{
  if (image->fp == NULL)
    return;

  fclose(image->fp);
  image->fp = NULL;
}

image_t* image_new(const char* filename)
{
  image_t* image;
  image = calloc(1, sizeof(image_t));
  image->filename = strdup(filename);
  image->fp = NULL;
  image->layout.address = NULL;
  image->size = -1;
  image->width = -1;
  image->height = -1;
  return image;
}

void image_free(image_t* image)
{
  if (image == NULL)
    return;

  if (image->layout.address != NULL)
  {
    free(image->layout.address);
    image->layout.address = NULL;
  }

  free(image->filename);
  free(image);
}

static void transform_to_rgb32(png_struct* png, png_row_info* row_info, png_byte* data)
{
  unsigned int i;

  for (i = 0; i < row_info->rowbytes; i += 4)
  {
    uint8_t red, green, blue, alpha;
    uint32_t pixel_value;
    red = data[i + 0];
    green = data[i + 1];
    blue = data[i + 2];
    alpha = data[i + 3];
    pixel_value = (alpha << 24) | (red << 16) | (green << 8) | (blue << 0);
    memcpy(data + i, &pixel_value, sizeof(uint32_t));
  }
}

bool image_load(image_t* image)
{
  png_struct* png;
  png_info* info;
  png_uint_32 width, height, bytes_per_row, row;
  int bits_per_pixel, color_type, interlace_method;
  png_byte** rows;

  if (!image_open_file(image))
    return false;

  png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  info = png_create_info_struct(png);
  png_init_io(png, image->fp);

  if (setjmp(png_jmpbuf(png)) != 0)
  {
    image_close_file(image);
    return false;
  }

  png_read_info(png, info);
  png_get_IHDR(png, info, &width, &height, &bits_per_pixel, &color_type, &interlace_method, NULL, NULL);
  bytes_per_row = 4 * width;

  if (color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png);

  if ((color_type == PNG_COLOR_TYPE_GRAY) && (bits_per_pixel < 8))
    png_set_expand_gray_1_2_4_to_8(png);

  if (png_get_valid(png, info, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png);

  if (bits_per_pixel == 16)
    png_set_strip_16(png);

  if (bits_per_pixel < 8)
    png_set_packing(png);

  if ((color_type == PNG_COLOR_TYPE_GRAY)
      || (color_type == PNG_COLOR_TYPE_GRAY_ALPHA))
    png_set_gray_to_rgb(png);

  if (interlace_method != PNG_INTERLACE_NONE)
    png_set_interlace_handling(png);

  png_set_filler(png, 0xff, PNG_FILLER_AFTER);
  png_set_read_user_transform_fn(png, transform_to_rgb32);
  png_read_update_info(png, info);
  rows = malloc(height * sizeof(png_byte*));
  image->layout.address = malloc(height * bytes_per_row);

  for (row = 0; row < height; row++)
    rows[row] = &image->layout.as_png_bytes[row * bytes_per_row];

  png_read_image(png, rows);
  free(rows);
  png_read_end(png, info);
  image_close_file(image);
  png_destroy_read_struct(&png, &info, NULL);
  image->width = width;
  image->height = height;
  return true;
}

uint32_t* image_get_data(image_t* image)
{
  return image->layout.as_pixels;
}

long image_get_width(image_t* image)
{
  return image->width;
}

long image_get_height(image_t* image)
{
  return image->height;
}

image_t* image_resize(image_t* image, long width, long height)
{
  image_t* new_image;
  int x, y;
  int old_x, old_y, old_width, old_height;
  float scale_x, scale_y;
  new_image = image_new(image->filename);
  new_image->layout.address = malloc(height * width * 4);
  new_image->width = width;
  new_image->height = height;
  old_width = image_get_width(image);
  old_height = image_get_height(image);
  scale_x = ((double) old_width) / width;
  scale_y = ((double) old_height) / height;

  for (y = 0; y < height; y++)
  {
    old_y = y * scale_y;

    for (x = 0; x < width; x++)
    {
      old_x = x * scale_x;
      new_image->layout.as_pixels[x + y * width] = image->layout.as_pixels[old_x + old_y * old_width];
    }
  }

  return new_image;
}

static int console_fd;

static void animate_at_time(fb_t* buf, image_t* image)
{
  fb_area_t area;
  uint32_t* data;
  long width, height;
  data = image_get_data(image);
  width = image_get_width(image);
  height = image_get_height(image);
  fb_get_size(buf, &area);
  area.x = (area.width / 2) - (width / 2);
  area.y = (area.height / 2) - (height / 2);
  area.width = width;
  area.height = height;
  fb_fill_with_argb32_data(buf, &area, 0, 0, data);
}

static void flush_generic(fb_t* buf)
{
  unsigned long row;
  char* row_buf;
  size_t bytes_per_row;
  unsigned long x1, y1, x2, y2;
  x1 = buf->area_to_flush.x;
  y1 = buf->area_to_flush.y;
  x2 = x1 + buf->area_to_flush.width;
  y2 = y1 + buf->area_to_flush.height;
  bytes_per_row = buf->area_to_flush.width * buf->bpp;
  row_buf = malloc(buf->row_stride * buf->bpp);

  for (row = y1; row < y2; row++)
  {
    unsigned long offset;
    offset = row * buf->row_stride * buf->bpp + x1 * buf->bpp;
    memcpy(buf->map_addr + offset, &buf->shadow_buf[row * buf->area.width + x1],
           buf->area_to_flush.width * buf->bpp);
  }

  free(row_buf);
}

static void flush_xrgb32(fb_t* buf)
{
  unsigned long x1, y1, x2, y2, y;
  char* dst, *src;
  x1 = buf->area_to_flush.x;
  y1 = buf->area_to_flush.y;
  x2 = x1 + buf->area_to_flush.width;
  y2 = y1 + buf->area_to_flush.height;
  dst = &buf->map_addr[(y1 * buf->row_stride + x1) * 4];
  src = (char*) &buf->shadow_buf[y1 * buf->area.width + x1];

  if (buf->area_to_flush.width == buf->row_stride)
  {
    memcpy(dst, src, buf->area_to_flush.width * buf->area_to_flush.height * 4);
    return;
  }

  for (y = y1; y < y2; y++)
  {
    memcpy(dst, src, buf->area_to_flush.width * 4);
    dst += buf->row_stride * 4;
    src += buf->area.width * 4;
  }
}

static bool fb_query_device(fb_t* buf)
{
  struct fb_var_screeninfo var_screen_info;
  struct fb_fix_screeninfo fix_screen_info;

  if (ioctl(buf->device_fd, FBIOGET_VSCREENINFO, &var_screen_info) < 0)
    return false;

  if (ioctl(buf->device_fd, FBIOGET_FSCREENINFO, &fix_screen_info) < 0)
    return false;

  /* Normally the pixel is divided into channels between the color components.
   * Each channel directly maps to a color channel on the hardware.
   *
   * There are some odd ball modes that use an indexed palette instead. In
   * those cases (pseudocolor, direct color, etc), the pixel value is just an
   * index into a lookup table of the real color values.
   *
   * We don't support that.
   */
  if (fix_screen_info.visual != FB_VISUAL_TRUECOLOR)
  {
    int rc = -1;
    int i;
    int depths[] = {32, 24, 16, 0};

    for (i = 0; depths[i] != 0; i++)
    {
      var_screen_info.bits_per_pixel = depths[i];
      var_screen_info.activate |= FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
      rc = ioctl(buf->device_fd, FBIOPUT_VSCREENINFO, &var_screen_info);

      if (rc >= 0)
      {
        if (ioctl(buf->device_fd, FBIOGET_FSCREENINFO, &fix_screen_info) < 0)
          return false;

        if (fix_screen_info.visual == FB_VISUAL_TRUECOLOR)
          break;
      }
    }

    if (ioctl(buf->device_fd, FBIOGET_VSCREENINFO, &var_screen_info) < 0)
      return false;

    if (ioctl(buf->device_fd, FBIOGET_FSCREENINFO, &fix_screen_info) < 0)
      return false;
  }

  if (fix_screen_info.visual != FB_VISUAL_TRUECOLOR || var_screen_info.bits_per_pixel < 16)
    return false;

  buf->area.x = var_screen_info.xoffset;
  buf->area.y = var_screen_info.yoffset;
  buf->area.width = var_screen_info.xres;
  buf->area.height = var_screen_info.yres;
  buf->r_bit_pos = var_screen_info.red.offset;
  buf->bits_for_r = var_screen_info.red.length;
  buf->g_bit_pos = var_screen_info.green.offset;
  buf->bits_for_g = var_screen_info.green.length;
  buf->b_bit_pos = var_screen_info.blue.offset;
  buf->bits_for_b = var_screen_info.blue.length;
  buf->a_bit_pos = var_screen_info.transp.offset;
  buf->bits_for_a = var_screen_info.transp.length;
  buf->bpp = var_screen_info.bits_per_pixel >> 3;
  buf->row_stride = fix_screen_info.line_length / buf->bpp;
  buf->size = buf->area.height * buf->row_stride * buf->bpp;

  if (buf->bpp == 4 &&
      buf->r_bit_pos == 16 && buf->bits_for_r == 8 &&
      buf->g_bit_pos == 8 && buf->bits_for_g == 8 &&
      buf->b_bit_pos == 0 && buf->bits_for_b == 8)
    buf->flush = flush_xrgb32;
  else
    buf->flush = flush_generic;

  return true;
}

static bool fb_map_to_device(fb_t* buf)
{
  buf->map_addr = mmap(NULL, buf->size, PROT_WRITE, MAP_SHARED, buf->device_fd, 0);
  return buf->map_addr != MAP_FAILED;
}

bool fb_open(fb_t* buf)
{
  bool is_open;
  is_open = false;

  if (!fb_open_device(buf))
  {
    goto out;
  }

  if (!fb_query_device(buf))
  {
    goto out;
  }

  if (!fb_map_to_device(buf))
  {
    goto out;
  }

  buf->shadow_buf = realloc(buf->shadow_buf, 4 * buf->area.width * buf->area.height);
  memset(buf->shadow_buf, 0, 4 * buf->area.width * buf->area.height);
  is_open = true;
out:

  if (!is_open)
  {
    int saved_errno;
    saved_errno = errno;
    fb_close_device(buf);
    errno = saved_errno;
  }

  return is_open;
}

int main(int argc, char** argv)
{
  image_t* image;
  fb_t* buf;
  int exit_code;
  exit_code = 0;

  if (argc == 1)
    image = image_new("/splash.png");
  else
    image = image_new(argv[1]);

  if (!image_load(image))
  {
    exit_code = errno;
    perror("could not load image");
    return exit_code;
  }

  console_fd = open("/dev/tty0", O_RDWR);
  buf = fb_new(NULL);

  if (!fb_open(buf))
  {
    exit_code = errno;
    perror("could not open framebuf");
    return exit_code;
  }

  image = image_resize(image, buf->area.width, buf->area.height);
  animate_at_time(buf, image);
  fb_close(buf);
  fb_free(buf);
  image_free(image);
  return exit_code;
}
