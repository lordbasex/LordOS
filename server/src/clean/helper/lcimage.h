/* lcimage.h — see lcimage.c. */
#ifndef LORD_CLEAN_IMAGE_H
#define LORD_CLEAN_IMAGE_H

#include <stdint.h>

/* The 64-bit difference hash of a grey image; *flat is set when it has almost no contrast. */
uint64_t lc_dhash(const unsigned char *grey, int w, int h, int *flat);
/* Reads and decodes an open file of size bytes. 0 with the hash and the size in pixels; 1 an image with
 * nothing to compare; -1 not one we can decode within the limits. */
int lc_image_hash(int fd, uint64_t size, uint64_t *hash, int *w, int *h);

#endif
