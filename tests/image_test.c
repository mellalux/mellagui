/* image-test FILE OUT.ppm: decode an image with client/image.c and write it as a PPM (alpha composited on white). */
#include <stdio.h>
#include <stdlib.h>

#include "image.h"

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n)
        return 2;
    fclose(f);
    char err[200] = "";
    struct image *im = image_decode(b, (size_t)n, err, sizeof(err));
    if (!im) {
        printf("ERR %s\n", err);
        return 1;
    }
    FILE *o = fopen(argv[2], "wb");
    fprintf(o, "P6\n%d %d\n255\n", im->w, im->h);
    for (int i = 0; i < im->w * im->h; i++) {
        uint32_t c = im->px[i], a = c >> 24;
        uint8_t rgb[3] = { (uint8_t)((c >> 16) & 255), (uint8_t)((c >> 8) & 255), (uint8_t)(c & 255) };
        for (int k = 0; k < 3; k++)
            rgb[k] = (uint8_t)((rgb[k] * a + 255 * (255 - a)) / 255);
        fwrite(rgb, 1, 3, o);
    }
    fclose(o);
    image_free(im);
    return 0;
}
