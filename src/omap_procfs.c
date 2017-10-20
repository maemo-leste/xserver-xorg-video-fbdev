/*
 * Copyright (c) 2008, 2009  Nokia Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fbdev.h"
#include <unistd.h>
#include <string.h>
#include <errno.h>

/* Is the character part of hex alphabet */
static int ishex(const char c)
{
	return  (c >= '0' && c <= '9') ||
		(c >= 'a' && c <= 'f') ||
		(c >= 'A' && c <= 'F');
}

/*
 * Grab the %u parameter from a string formatted
 * like this "    %x-%x (%u bytes)"
 */
static unsigned int omap_vram_get_size(const char *s)
{
	unsigned long size;

	while (*s != '\0' && *s != '(')
		s++;

	if (*s++ == '\0')
		return 0;

	errno = 0;
	size = strtoul(s, NULL, 10);
	if (errno)
		return 0;

	return size;
}

/*
 * Determine the amount of video memory available
 * by parsing /proc/omap-vram
 * The file format is:
 * "%x-%x (%u bytes)"      <- reserved area
 * "    %x-%x (%u bytes)"  <- allocated area
 * ...
 *
 * FIXME doesn't take fragmentation into account.
 */
_X_DEPRECATED unsigned int omap_vram_get_avail(void)
{
	FILE *f;
	unsigned int size = 0, used = 0, max = 0;

	f = fopen("/proc/omap-vram", "r");
	if (!f)
		return 0;

	for (;;) {
		char buf[64];
		char *s = fgets(buf, sizeof buf, f);
		if (!s)
			break;

		if (ishex(*s)) {
			if (size && size - used > max)
				max = size - used;
			size = omap_vram_get_size(s);
			used = 0;
		} else if (*s == ' ') {
			while (*s == ' ')
				s++;
			if (ishex(*s)) {
				used += omap_vram_get_size(s);
			}
		}
	}

	if (size && size - used > max)
		max = size - used;

	fclose(f);

	return max;
}
