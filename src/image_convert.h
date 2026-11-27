/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef IMAGE_CONVERT_H
#define IMAGE_CONVERT_H

#include <stddef.h>

/*
 * Converts the rectangle (src_x, src_y, width, height) of a 32-bit packed
 * frame into the destination image at its top-left corner, applying the
 * wl_output transform the compositor reported (0..7, 90/180/270 are
 * counter-clockwise, the flipped values are a flip followed by a rotation).
 *
 * dst is dst_w x dst_h pixels with dst_stride bytes per row.  src holds
 * src_size bytes with src_stride bytes per row.  swap_rb swaps the red and
 * blue bytes (RGBx/RGBA frames on a BGRx X11 image); the alpha byte is set
 * to 0xff when swapping.
 *
 * Pixels of the destination that are not covered by the source rectangle are
 * cleared, so a cropped or partial frame never leaves stale content behind.
 * Returns 0 when a rectangle was written (possibly empty), -1 on bad input.
 */
int image_convert(unsigned char *dst, unsigned int dst_w, unsigned int dst_h,
		  size_t dst_stride,
		  const unsigned char *src, size_t src_size, int src_stride,
		  int src_x, int src_y, unsigned int width, unsigned int height,
		  int swap_rb, int transform);

/*
 * Same as image_convert(), but the (rotated) source rectangle is scaled to
 * fill dst exactly.  Every destination pixel is the average of the source
 * pixels it covers, so downscaling keeps text readable instead of picking
 * single pixels.  Used for the quality presets, which cap the mirror
 * resolution.  The destination is always fully written; alpha is opaque.
 */
int image_convert_scaled(unsigned char *dst, unsigned int dst_w, unsigned int dst_h,
			 size_t dst_stride,
			 const unsigned char *src, size_t src_size, int src_stride,
			 int src_x, int src_y, unsigned int width, unsigned int height,
			 int swap_rb, int transform);

#endif /* IMAGE_CONVERT_H */
