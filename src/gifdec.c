#include "gifdec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define MIN(A, B) ((A) < (B) ? (A) : (B))
#define MAX(A, B) ((A) > (B) ? (A) : (B))

// File based
static off_t gd_lseek_file(gd_GIF *gif, off_t offset, int whence) {
  return lseek(gif->fd, offset, whence);
}

static int gd_read_file(gd_GIF *gif, void *buf, size_t count) {
  return read(gif->fd, buf, count);
}

// Memory based
static off_t gd_lseek_memory(gd_GIF *gif, off_t offset, int whence) {
  switch (whence) {
    case SEEK_SET:
      gif->gif_data_pos = MIN(MAX(0, offset), gif->gif_data_size);
      break;
    case SEEK_CUR:
      gif->gif_data_pos = MIN(MAX(0, gif->gif_data_pos + offset), gif->gif_data_size);
      break;
    case SEEK_END:
      gif->gif_data_pos = gif->gif_data_size;
      break;
  }
  return gif->gif_data_pos;
}

static int gd_read_memory(gd_GIF *gif, void *buf, size_t count) {
  count = MIN(count, gif->gif_data_size - gif->gif_data_pos);
  if (count == 0) {
    return -1;
  }
  memcpy(buf, gif->gif_data + gif->gif_data_pos, count);
  gif->gif_data_pos += count;
  return count;
}

typedef struct Entry {
    uint16_t length;
    uint16_t prefix;
    uint8_t  suffix;
} Entry;

typedef struct Table {
    int bulk;
    int nentries;
    Entry *entries;
} Table;

static uint16_t
read_num(gd_GIF *gif)
{
    uint8_t bytes[2];

    gif->gd_read(gif, bytes, 2);
    return bytes[0] + (((uint16_t) bytes[1]) << 8);
}

static int gd_open_gif_impl(gd_GIF *gif) {

  uint8_t sigver[3];
  uint16_t width, height, depth;
  uint8_t fdsz, bgidx, aspect;
  int i;
  uint8_t *bgcolor;
  int gct_sz;

  /* Header */
  gif->gd_read(gif, sigver, 3);
  if (memcmp(sigver, "GIF", 3) != 0) {
    GD_LOG_ERROR("invalid signature");
    return -1;
  }
  /* Version */
  gif->gd_read(gif, sigver, 3);
  if (memcmp(sigver, "89a", 3) != 0) {
    GD_LOG_ERROR("invalid version");
    return -1;
  }
  /* Width x Height */
  width  = read_num(gif);
  height = read_num(gif);
  /* FDSZ */
  gif->gd_read(gif, &fdsz, 1);
  /* Presence of GCT */
  if (!(fdsz & 0x80)) {
    GD_LOG_ERROR("no global color table");
    return -1;
  }
  /* Color Space's Depth */
  depth = ((fdsz >> 4) & 7) + 1;
  /* Ignore Sort Flag. */
  /* GCT Size */
  gct_sz = 1 << ((fdsz & 0x07) + 1);
  /* Background Color Index */
  gif->gd_read(gif, &bgidx, 1);
  /* Aspect Ratio */
  gif->gd_read(gif, &aspect, 1);
  /* Create gd_GIF Structure. */

  gif->width  = width;
  gif->height = height;
  gif->depth  = depth;
  /* Read GCT */
  gif->gct.size = gct_sz;
  gif->gd_read(gif, gif->gct.colors, 3 * gif->gct.size);
  gif->palette = &gif->gct;
  gif->bgindex = bgidx;
  gif->frame = (unsigned char*)GD_CALLOC(width * height, 4);
  if (!gif->frame) {
    GD_LOG_ERROR("Unable to allocate memory for frame buffer");
    return -1;
  }
  gif->canvas = &gif->frame[width * height];
  if (gif->bgindex)
    memset(gif->frame, gif->bgindex, gif->width * gif->height);
  bgcolor = &gif->palette->colors[gif->bgindex*3];
  if (bgcolor[0] || bgcolor[1] || bgcolor [2])
    for (i = 0; i < gif->width * gif->height; i++)
      memcpy(&gif->canvas[i*3], bgcolor, 3);
  gif->anim_start = gif->gd_lseek(gif, 0, SEEK_CUR);
  return 0;
}

gd_GIF *gd_open_gif_memory(const char *data, size_t size) {
  gd_GIF *gif;

  gif = (gd_GIF*)GD_CALLOC(1, sizeof(*gif));
  if (!gif) {
    GD_LOG_ERROR("Unable to allocate memory for gd_GIF");
    return 0;
  }

  gif->gif_data = data;
  gif->gif_data_size = size;
  gif->gif_data_pos = 0;
  gif->gd_lseek = gd_lseek_memory;
  gif->gd_read = gd_read_memory;

  if (gd_open_gif_impl(gif) != 0) {
    GD_FREE(gif);
    return 0;
  }
  return gif;
}

gd_GIF *
gd_open_gif(const char *fname)
{
    gd_GIF *gif;
    int fd;

    fd = open(fname, O_RDONLY);
    if (fd == -1) return NULL;
#ifdef _WIN32
    setmode(fd, O_BINARY);
#endif
    gif = (gd_GIF*)GD_CALLOC(1, sizeof(*gif));
    if (!gif) {
      GD_LOG_ERROR("Unable to allocate memory for gd_GIF");
      return 0;
    }

    gif->fd = fd;
    gif->gd_lseek = gd_lseek_file;
    gif->gd_read = gd_read_file;

    if (gd_open_gif_impl(gif) != 0) {
      GD_FREE(gif);
      close(fd);
      return 0;
    }
    return gif;
}

static void
discard_sub_blocks(gd_GIF *gif)
{
    uint8_t size;

    do {
        gif->gd_read(gif, &size, 1);
        gif->gd_lseek(gif, size, SEEK_CUR);
    } while (size);
}

static void
read_plain_text_ext(gd_GIF *gif)
{
    if (gif->plain_text) {
        uint16_t tx, ty, tw, th;
        uint8_t cw, ch, fg, bg;
        off_t sub_block;
        gif->gd_lseek(gif, 1, SEEK_CUR); /* block size = 12 */
        tx = read_num(gif);
        ty = read_num(gif);
        tw = read_num(gif);
        th = read_num(gif);
        gif->gd_read(gif, &cw, 1);
        gif->gd_read(gif, &ch, 1);
        gif->gd_read(gif, &fg, 1);
        gif->gd_read(gif, &bg, 1);
        sub_block = gif->gd_lseek(gif, 0, SEEK_CUR);
        gif->plain_text(gif, tx, ty, tw, th, cw, ch, fg, bg);
        gif->gd_lseek(gif, sub_block, SEEK_SET);
    } else {
        /* Discard plain text metadata. */
        gif->gd_lseek(gif, 13, SEEK_CUR);
    }
    /* Discard plain text sub-blocks. */
    discard_sub_blocks(gif);
}

static void
read_graphic_control_ext(gd_GIF *gif)
{
    uint8_t rdit;

    /* Discard block size (always 0x04). */
    gif->gd_lseek(gif, 1, SEEK_CUR);
    gif->gd_read(gif, &rdit, 1);
    gif->gce.disposal = (rdit >> 2) & 3;
    gif->gce.input = rdit & 2;
    gif->gce.transparency = rdit & 1;
    gif->gce.delay = read_num(gif);
    gif->gd_read(gif, &gif->gce.tindex, 1);
    /* Skip block terminator. */
    gif->gd_lseek(gif, 1, SEEK_CUR);
}

static void
read_comment_ext(gd_GIF *gif)
{
    if (gif->comment) {
        off_t sub_block = gif->gd_lseek(gif, 0, SEEK_CUR);
        gif->comment(gif);
        gif->gd_lseek(gif, sub_block, SEEK_SET);
    }
    /* Discard comment sub-blocks. */
    discard_sub_blocks(gif);
}

static void
read_application_ext(gd_GIF *gif)
{
    char app_id[8];
    char app_auth_code[3];

    /* Discard block size (always 0x0B). */
    gif->gd_lseek(gif, 1, SEEK_CUR);
    /* Application Identifier. */
    gif->gd_read(gif, app_id, 8);
    /* Application Authentication Code. */
    gif->gd_read(gif, app_auth_code, 3);
    if (!strncmp(app_id, "NETSCAPE", sizeof(app_id))) {
        /* Discard block size (0x03) and constant byte (0x01). */
        gif->gd_lseek(gif, 2, SEEK_CUR);
        gif->loop_count = read_num(gif);
        /* Skip block terminator. */
        gif->gd_lseek(gif, 1, SEEK_CUR);
    } else if (gif->application) {
        off_t sub_block = gif->gd_lseek(gif, 0, SEEK_CUR);
        gif->application(gif, app_id, app_auth_code);
        gif->gd_lseek(gif, sub_block, SEEK_SET);
        discard_sub_blocks(gif);
    } else {
        discard_sub_blocks(gif);
    }
}

static void
read_ext(gd_GIF *gif)
{
    uint8_t label;

    gif->gd_read(gif, &label, 1);
    switch (label) {
    case 0x01:
        read_plain_text_ext(gif);
        break;
    case 0xF9:
        read_graphic_control_ext(gif);
        break;
    case 0xFE:
        read_comment_ext(gif);
        break;
    case 0xFF:
        read_application_ext(gif);
        break;
    default:
        GD_LOG_ERROR("unknown extension: %02X", label);
    }
}

static Table *
new_table(int key_size)
{
    int key;
    int init_bulk = MAX(1 << (key_size + 1), 0x100);
    Table *table = (Table*)GD_MALLOC(sizeof(*table) + sizeof(Entry) * init_bulk);
    if (table) {
        table->bulk = init_bulk;
        table->nentries = (1 << key_size) + 2;
        table->entries = (Entry *) &table[1];
        for (key = 0; key < (1 << key_size); key++)
            table->entries[key] = (Entry) {1, 0xFFF, (uint8_t)key};
    } else {
      GD_LOG_ERROR("Unable to allocate memory for LZW code table: %d %p", (sizeof(*table) + sizeof(Entry) * init_bulk), table);
    }
    return table;
}

/* Add table entry. Return value:
 *  0 on success
 *  +1 if key size must be incremented after this addition
 *  -1 if could not realloc table */
static int
add_entry(Table **tablep, uint16_t length, uint16_t prefix, uint8_t suffix)
{
    Table *table = *tablep;
    if (table->nentries == table->bulk) {
        table->bulk *= 2;
        table = (Table*)realloc(table, sizeof(*table) + sizeof(Entry) * table->bulk);
        if (!table) return -1;
        table->entries = (Entry *) &table[1];
        *tablep = table;
    }
    table->entries[table->nentries] = (Entry) {length, prefix, suffix};
    table->nentries++;
    if ((table->nentries & (table->nentries - 1)) == 0)
        return 1;
    return 0;
}

static uint16_t
get_key(gd_GIF *gif, int key_size, uint8_t *sub_len, uint8_t *shift, uint8_t *byte)
{
    int bits_read;
    int rpad;
    int frag_size;
    uint16_t key;

    key = 0;
    for (bits_read = 0; bits_read < key_size; bits_read += frag_size) {
        rpad = (*shift + bits_read) % 8;
        if (rpad == 0) {
            /* Update byte. */
            if (*sub_len == 0) {
                gif->gd_read(gif, sub_len, 1); /* Must be nonzero! */
                if (*sub_len == 0)
                    return 0x1000;
            }
            gif->gd_read(gif, byte, 1);
            (*sub_len)--;
        }
        frag_size = MIN(key_size - bits_read, 8 - rpad);
        key |= ((uint16_t) ((*byte) >> rpad)) << bits_read;
    }
    /* Clear extra bits to the left. */
    key &= (1 << key_size) - 1;
    *shift = (*shift + key_size) % 8;
    return key;
}

/* Compute output index of y-th input line, in frame of height h. */
static int
interlaced_line_index(int h, int y)
{
    int p; /* number of lines in current pass */

    p = (h - 1) / 8 + 1;
    if (y < p) /* pass 1 */
        return y * 8;
    y -= p;
    p = (h - 5) / 8 + 1;
    if (y < p) /* pass 2 */
        return y * 8 + 4;
    y -= p;
    p = (h - 3) / 4 + 1;
    if (y < p) /* pass 3 */
        return y * 4 + 2;
    y -= p;
    /* pass 4 */
    return y * 2 + 1;
}

/* Decompress image pixels.
 * Return 0 on success or -1 on out-of-memory (w.r.t. LZW code table). */
static int
read_image_data(gd_GIF *gif, int interlace)
{
    uint8_t sub_len, shift, byte;
    int init_key_size, key_size, table_is_full = 0;
    int frm_off, frm_size, str_len = 0, i, p, x, y;
    uint16_t key, clear, stop;
    int ret;
    Table *table;
    Entry entry = {0, 0xFFF, 0};
    off_t start, end;

    gif->gd_read(gif, &byte, 1);
    key_size = (int) byte;
    if (key_size < 2 || key_size > 8)
        return -1;
    
    start = gif->gd_lseek(gif, 0, SEEK_CUR);
    discard_sub_blocks(gif);
    end = gif->gd_lseek(gif, 0, SEEK_CUR);
    gif->gd_lseek(gif, start, SEEK_SET);
    clear = 1 << key_size;
    stop = clear + 1;
    table = new_table(key_size);
    if (!table) {
      return -1;
    }
    key_size++;
    init_key_size = key_size;
    sub_len = shift = 0;
    key = get_key(gif, key_size, &sub_len, &shift, &byte); /* clear code */
    frm_off = 0;
    ret = 0;
    frm_size = gif->fw*gif->fh;
    while (frm_off < frm_size) {
        if (key == clear) {
            key_size = init_key_size;
            table->nentries = (1 << (key_size - 1)) + 2;
            table_is_full = 0;
        } else if (!table_is_full) {
            ret = add_entry(&table, str_len + 1, key, entry.suffix);
            if (ret == -1) {
                GD_FREE(table);
                return -1;
            }
            if (table->nentries == 0x1000) {
                ret = 0;
                table_is_full = 1;
            }
        }
        key = get_key(gif, key_size, &sub_len, &shift, &byte);
        if (key == clear) continue;
        if (key == stop || key == 0x1000) break;
        if (ret == 1) key_size++;
        entry = table->entries[key];
        str_len = entry.length;
        for (i = 0; i < str_len; i++) {
            p = frm_off + entry.length - 1;
            x = p % gif->fw;
            y = p / gif->fw;
            if (interlace)
                y = interlaced_line_index((int) gif->fh, y);
            gif->frame[(gif->fy + y) * gif->width + gif->fx + x] = entry.suffix;
            if (entry.prefix == 0xFFF)
                break;
            else
                entry = table->entries[entry.prefix];
        }
        frm_off += str_len;
        if (key < table->nentries - 1 && !table_is_full)
            table->entries[table->nentries - 1].suffix = entry.suffix;
    }
    GD_FREE(table);
    if (key == stop)
        gif->gd_read(gif, &sub_len, 1); /* Must be zero! */
    gif->gd_lseek(gif, end, SEEK_SET);
    return 0;
}

/* Read image.
 * Return 0 on success or -1 on out-of-memory (w.r.t. LZW code table). */
static int
read_image(gd_GIF *gif)
{
    uint8_t fisrz;
    int interlace;

    /* Image Descriptor. */
    gif->fx = read_num(gif);
    gif->fy = read_num(gif);
    
    if (gif->fx >= gif->width || gif->fy >= gif->height)
        return -1;
    
    gif->fw = read_num(gif);
    gif->fh = read_num(gif);
    
    gif->fw = MIN(gif->fw, gif->width - gif->fx);
    gif->fh = MIN(gif->fh, gif->height - gif->fy);
    
    gif->gd_read(gif, &fisrz, 1);
    interlace = fisrz & 0x40;
    /* Ignore Sort Flag. */
    /* Local Color Table? */
    if (fisrz & 0x80) {
        /* Read LCT */
        gif->lct.size = 1 << ((fisrz & 0x07) + 1);
        gif->gd_read(gif, gif->lct.colors, 3 * gif->lct.size);
        gif->palette = &gif->lct;
    } else
        gif->palette = &gif->gct;
    /* Image Data. */
    return read_image_data(gif, interlace);
}

static void
render_frame_rect(gd_GIF *gif, uint8_t *buffer)
{
    int i, j, k;
    uint8_t index, *color;
    i = gif->fy * gif->width + gif->fx;
    for (j = 0; j < gif->fh; j++) {
        for (k = 0; k < gif->fw; k++) {
            index = gif->frame[(gif->fy + j) * gif->width + gif->fx + k];
            color = &gif->palette->colors[index*3];
            if (!gif->gce.transparency || index != gif->gce.tindex)
                memcpy(&buffer[(i+k)*3], color, 3);
        }
        i += gif->width;
    }
}

static void
dispose(gd_GIF *gif)
{
    int i, j, k;
    uint8_t *bgcolor;
    switch (gif->gce.disposal) {
    case 2: /* Restore to background color. */
        bgcolor = &gif->palette->colors[gif->bgindex*3];
        i = gif->fy * gif->width + gif->fx;
        for (j = 0; j < gif->fh; j++) {
            for (k = 0; k < gif->fw; k++)
                memcpy(&gif->canvas[(i+k)*3], bgcolor, 3);
            i += gif->width;
        }
        break;
    case 3: /* Restore to previous, i.e., don't update canvas.*/
        break;
    default:
        /* Add frame non-transparent pixels to canvas. */
        render_frame_rect(gif, gif->canvas);
    }
}

/* Return 1 if got a frame; 0 if got GIF trailer; -1 if error. */
int
gd_get_frame(gd_GIF *gif)
{
    char sep;

    dispose(gif);
    gif->gd_read(gif, &sep, 1);
    while (sep != ',') {
        if (sep == ';')
            return 0;
        if (sep == '!')
            read_ext(gif);
        else return -1;
        gif->gd_read(gif, &sep, 1);
    }
    if (read_image(gif) == -1)
        return -1;
    return 1;
}

void
gd_render_frame(gd_GIF *gif, uint8_t *buffer)
{
    memcpy(buffer, gif->canvas, gif->width * gif->height * 3);
    render_frame_rect(gif, buffer);
}

int gd_is_bgcolor(gd_GIF *gif, uint8_t color[3]) {
  return !memcmp(&gif->palette->colors[gif->bgindex * 3], color, 3);
}

off_t
gd_rewind(gd_GIF *gif)
{
    return gif->gd_lseek(gif, gif->anim_start, SEEK_SET);
}

void
gd_close_gif(gd_GIF *gif)
{
  if (gif->fd != -1) {
    close(gif->fd);
  }
  if (gif->frame) {
    GD_FREE(gif->frame);
  }
  GD_FREE(gif);
}
