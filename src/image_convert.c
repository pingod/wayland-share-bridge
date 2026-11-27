/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * image_convert.c - copy/rotate/swap a captured frame into the X11 image.
 *
 * The conversion is kept free of Xlib and PipeWire so tests/ can verify the
 * rotation and crop arithmetic on small synthetic frames.
 */

#include <stdint.h>
#include <string.h>

#include "image_convert.h"

static void clear_image(unsigned char *dst, unsigned int dst_w, unsigned int dst_h,
			size_t dst_stride) {
	unsigned int y;

	for(y = 0; y < dst_h; y++)
		memset(dst + (size_t) y * dst_stride, 0, (size_t) dst_w * 4);
}

static void copy_pixel(unsigned char *dst, const unsigned char *src, int swap_rb) {
	if(swap_rb) {
		dst[0] = src[2];
		dst[1] = src[1];
		dst[2] = src[0];
		dst[3] = 0xff;
	} else {
		memcpy(dst, src, 4);
	}
}

/*
 * Maps a destination pixel back to the source rectangle after applying the
 * wl_output transform (90/180/270 are counter-clockwise, the flipped values
 * are a vertical-axis flip followed by the rotation).
 */
static void map_pixel(int transform, int x, int y, int width, int height,
		      int *sx, int *sy) {
	switch(transform) {
	case 1: /* 90 degrees counter-clockwise */
		*sx = width - 1 - y;
		*sy = x;
		break;
	case 2: /* 180 degrees */
		*sx = width - 1 - x;
		*sy = height - 1 - y;
		break;
	case 3: /* 270 degrees counter-clockwise */
		*sx = y;
		*sy = height - 1 - x;
		break;
	case 4: /* flip around the vertical axis */
		*sx = width - 1 - x;
		*sy = y;
		break;
	case 5: /* flip, then 90 degrees counter-clockwise (transpose) */
		*sx = y;
		*sy = x;
		break;
	case 6: /* flip, then 180 degrees (vertical mirror) */
		*sx = x;
		*sy = height - 1 - y;
		break;
	case 7: /* flip, then 270 degrees counter-clockwise */
		*sx = width - 1 - y;
		*sy = height - 1 - x;
		break;
	default: /* no transform */
		*sx = x;
		*sy = y;
		break;
	}
}

int image_convert(unsigned char *dst, unsigned int dst_w, unsigned int dst_h,
		  size_t dst_stride,
		  const unsigned char *src, size_t src_size, int src_stride,
		  int src_x, int src_y, unsigned int width, unsigned int height,
		  int swap_rb, int transform) {
	int rotated = (transform == 1 || transform == 3 ||
		       transform == 5 || transform == 7);
	int cols_avail, rows_avail;
	unsigned int out_w, out_h, x, y;

	if(!dst || !src || dst_w == 0 || dst_h == 0 || dst_stride < (size_t) dst_w * 4)
		return -1;
	if(src_stride <= 0 || src_size == 0)
		return -1;
	if(transform < 0 || transform > 7)
		transform = 0;

	cols_avail = src_stride / 4;
	rows_avail = (int) (src_size / (size_t) src_stride);
	if(src_x < 0)
		src_x = 0;
	if(src_y < 0)
		src_y = 0;
	if(src_x >= cols_avail || src_y >= rows_avail) {
		clear_image(dst, dst_w, dst_h, dst_stride);
		return 0;
	}

	if(width > (unsigned int) (cols_avail - src_x))
		width = (unsigned int) (cols_avail - src_x);
	if(height > (unsigned int) (rows_avail - src_y))
		height = (unsigned int) (rows_avail - src_y);

	if(rotated) {
		/* The destination is transpose(width, height). */
		if(height > dst_w)
			height = dst_w;
		if(width > dst_h)
			width = dst_h;
		out_w = height;
		out_h = width;
	} else {
		if(width > dst_w)
			width = dst_w;
		if(height > dst_h)
			height = dst_h;
		out_w = width;
		out_h = height;
	}

	if(out_w == 0 || out_h == 0) {
		clear_image(dst, dst_w, dst_h, dst_stride);
		return 0;
	}

	if(transform == 0 && !swap_rb) {
		/* Fast path: plain copy of the (possibly cropped) rows. */
		for(y = 0; y < out_h; y++) {
			unsigned char *d = dst + (size_t) y * dst_stride;
			const unsigned char *s = src + (size_t) (src_y + (int) y) * src_stride +
						 (size_t) src_x * 4;

			memcpy(d, s, (size_t) out_w * 4);
			if(out_w < dst_w)
				memset(d + (size_t) out_w * 4, 0, (size_t) (dst_w - out_w) * 4);
		}
	} else {
		for(y = 0; y < out_h; y++) {
			unsigned char *d = dst + (size_t) y * dst_stride;

			for(x = 0; x < out_w; x++) {
				int sx, sy;

				map_pixel(transform, (int) x, (int) y, (int) width, (int) height,
					  &sx, &sy);
				copy_pixel(d + (size_t) x * 4,
					   src + (size_t) (src_y + sy) * src_stride +
					   (size_t) (src_x + sx) * 4,
					   swap_rb);
			}
			if(out_w < dst_w)
				memset(d + (size_t) out_w * 4, 0, (size_t) (dst_w - out_w) * 4);
		}
	}

	if(out_h < dst_h)
		memset(dst + (size_t) out_h * dst_stride, 0,
		       (size_t) (dst_h - out_h) * dst_stride);

	return 0;
}

/*
 * Edges of the source area covered by destination pixel [index, index + 1):
 * [index * out / dst, (index + 1) * out / dst), widened to whole pixels.
 * At a 1:1 scale this gives exactly one source pixel per destination pixel.
 */
static void sample_range(unsigned int index, unsigned int out, unsigned int dst,
			 unsigned int *first, unsigned int *last) {
	*first = (unsigned int) ((uint64_t) index * out / dst);
	*last = (unsigned int) (((uint64_t) (index + 1) * out + dst - 1) / dst);
	if(*last > out)
		*last = out;
	if(*last <= *first)
		*last = *first + 1;
}

int image_convert_scaled(unsigned char *dst, unsigned int dst_w, unsigned int dst_h,
			 size_t dst_stride,
			 const unsigned char *src, size_t src_size, int src_stride,
			 int src_x, int src_y, unsigned int width, unsigned int height,
			 int swap_rb, int transform) {
	int rotated, cols_avail, rows_avail;
	unsigned int out_w, out_h, x, y;

	if(!dst || !src || dst_w == 0 || dst_h == 0 || dst_stride < (size_t) dst_w * 4)
		return -1;
	if(src_stride <= 0 || src_size == 0)
		return -1;
	if(transform < 0 || transform > 7)
		transform = 0;

	cols_avail = src_stride / 4;
	rows_avail = (int) (src_size / (size_t) src_stride);
	if(src_x < 0)
		src_x = 0;
	if(src_y < 0)
		src_y = 0;
	if(src_x >= cols_avail || src_y >= rows_avail) {
		clear_image(dst, dst_w, dst_h, dst_stride);
		return 0;
	}

	if(width > (unsigned int) (cols_avail - src_x))
		width = (unsigned int) (cols_avail - src_x);
	if(height > (unsigned int) (rows_avail - src_y))
		height = (unsigned int) (rows_avail - src_y);

	rotated = (transform == 1 || transform == 3 ||
		   transform == 5 || transform == 7);
	out_w = rotated ? height : width;
	out_h = rotated ? width : height;
	if(out_w == 0 || out_h == 0) {
		clear_image(dst, dst_w, dst_h, dst_stride);
		return 0;
	}

	if(dst_w == out_w && dst_h == out_h)
		return image_convert(dst, dst_w, dst_h, dst_stride,
				     src, src_size, src_stride,
				     src_x, src_y, width, height, swap_rb, transform);

	for(y = 0; y < dst_h; y++) {
		unsigned char *d = dst + (size_t) y * dst_stride;
		unsigned int ry0, ry1, ry;

		sample_range(y, out_h, dst_h, &ry0, &ry1);
		for(x = 0; x < dst_w; x++) {
			unsigned int rx0, rx1, rx, sum[3] = { 0, 0, 0 };
			unsigned int count = 0;

			sample_range(x, out_w, dst_w, &rx0, &rx1);
			for(ry = ry0; ry < ry1; ry++) {
				for(rx = rx0; rx < rx1; rx++) {
					const unsigned char *p;
					int sx, sy;

					if(transform == 0) {
						sx = (int) rx;
						sy = (int) ry;
					} else {
						map_pixel(transform, (int) rx, (int) ry,
							  (int) width, (int) height,
							  &sx, &sy);
					}
					p = src + (size_t) (src_y + sy) * src_stride +
					    (size_t) (src_x + sx) * 4;
					sum[0] += p[0];
					sum[1] += p[1];
					sum[2] += p[2];
					count++;
				}
			}
			if(count) {
				sum[0] /= count;
				sum[1] /= count;
				sum[2] /= count;
			}
			if(swap_rb) {
				d[0] = (unsigned char) sum[2];
				d[1] = (unsigned char) sum[1];
				d[2] = (unsigned char) sum[0];
			} else {
				d[0] = (unsigned char) sum[0];
				d[1] = (unsigned char) sum[1];
				d[2] = (unsigned char) sum[2];
			}
			d[3] = 0xff;
			d += 4;
		}
	}

	return 0;
}
