#ifndef GIFDEC_H
#define GIFDEC_H

#include <stdint.h>
#include <sys/types.h>

#ifndef GD_CALLOC
#define GD_CALLOC(n, s) calloc(n, s)
#endif

#ifndef GD_FREE
#define GD_FREE(p) free(p)
#endif

#ifndef GD_MALLOC
#define GD_MALLOC(s) malloc(s)
#endif

#ifndef GD_LOG_ERROR
#define GD_LOG_ERROR(f, ...) fprintf(stderr, f "\n", ##__VA_ARGS__)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gd_Palette {
    int size;
    uint8_t colors[0x100 * 3];
} gd_Palette;

typedef struct gd_GCE {
    uint16_t delay;
    uint8_t tindex;
    uint8_t disposal;
    int input;
    int transparency;
} gd_GCE;

typedef struct gd_GIF {
    // file based
    int fd;

    // memory based
    const char* gif_data;
    size_t gif_data_size;
    off_t gif_data_pos;

    off_t anim_start;
    uint16_t width, height;
    uint16_t depth;
    uint16_t loop_count;
    gd_GCE gce;
    gd_Palette *palette;
    gd_Palette lct, gct;
    void (*plain_text)(
        struct gd_GIF *gif, uint16_t tx, uint16_t ty,
        uint16_t tw, uint16_t th, uint8_t cw, uint8_t ch,
        uint8_t fg, uint8_t bg
    );
    void (*comment)(struct gd_GIF *gif);
    void (*application)(struct gd_GIF *gif, char id[8], char auth[3]);
    uint16_t fx, fy, fw, fh;
    uint8_t bgindex;
    uint8_t *canvas, *frame;

    off_t (*gd_lseek)(struct gd_GIF *gif, off_t offset, int whence);
    int (*gd_read)(struct gd_GIF *gif, void *__buf, size_t __nbyte);
} gd_GIF;

gd_GIF *gd_open_gif(const char *fname);
gd_GIF *gd_open_gif_memory(const char *data, size_t size);
int gd_get_frame(gd_GIF *gif);
void gd_render_frame(gd_GIF *gif, uint8_t *buffer);
int gd_is_bgcolor(gd_GIF *gif, uint8_t color[3]);
off_t gd_rewind(gd_GIF *gif);
void gd_close_gif(gd_GIF *gif);

#ifdef __cplusplus
}
#endif

#endif /* GIFDEC_H */
