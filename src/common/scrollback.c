#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE   /* also set globally by the Makefile */
#endif
#include "scrollback.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/path.h"
#include "common/proto.h"
#include "common/xutil.h"

/* ---- crc32 (IEEE, table-driven) ---- */

static uint32_t crc_table[256];
static bool crc_ready;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    crc_ready = true;
}

static uint32_t crc32_buf(const void *data, size_t n) {
    if (!crc_ready) crc_init();
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---- in-memory ring of serialized lines ---- */

typedef struct {
    char *text;    /* malloc'd */
    uint32_t len;
    uint64_t seq;
} mem_line;

struct scrollback {
    char dir[600];       /* sessions/<name>/ */
    char log_path[640];
    int fd;              /* append-only log */
    uint64_t next_seq;
    uint32_t file_max;
    uint64_t file_size;  /* bytes written to current log */

    mem_line *ring;      /* mem_lines entries */
    uint32_t mem_lines, ring_head, ring_count;

    uint8_t wbuf[64 * 1024]; /* write coalescing */
    size_t wbuf_len;
};

/* ---- serialization: cells → ANSI text ---- */

static size_t serialize_line(const vt_cell *cells, uint16_t n, char *out, size_t cap) {
    /* Emit SGR only on change; trim trailing empty cells. */
    uint16_t last = n;
    while (last > 0 && cells[last - 1].cp == 0 &&
           !(cells[last - 1].attrs & VT_ATTR_WIDE_SPACER))
        last--;

    size_t o = 0;
    uint32_t fg = VT_COLOR_DEFAULT, bg = VT_COLOR_DEFAULT;
    uint16_t attrs = 0;
    bool dirty = false;

#define PUT(s, l) do { if (o + (l) > cap) return o; memcpy(out + o, (s), (l)); o += (l); } while (0)
#define PUTF(...) do { char t[48]; int r = snprintf(t, sizeof t, __VA_ARGS__); \
                       if (r > 0) PUT(t, (size_t)r); } while (0)

    for (uint16_t c = 0; c < last; c++) {
        const vt_cell *cell = &cells[c];
        if (cell->attrs & VT_ATTR_WIDE_SPACER) continue;
        uint16_t a = cell->attrs & (uint16_t)~(VT_ATTR_WIDE | VT_ATTR_WIDE_SPACER);
        if (a != attrs || cell->fg != fg || cell->bg != bg || (dirty && 0)) {
            PUT("\x1b[0m", 4);
            if (a & VT_ATTR_BOLD) PUT("\x1b[1m", 4);
            if (a & VT_ATTR_DIM) PUT("\x1b[2m", 4);
            if (a & VT_ATTR_ITALIC) PUT("\x1b[3m", 4);
            if (a & VT_ATTR_UNDERLINE) PUT("\x1b[4m", 4);
            if (a & VT_ATTR_REVERSE) PUT("\x1b[7m", 4);
            if (a & VT_ATTR_STRIKE) PUT("\x1b[9m", 4);
            if (cell->fg != VT_COLOR_DEFAULT) {
                if ((cell->fg & 0xFF000000u) == 0x01000000u) PUTF("\x1b[38;5;%um", cell->fg & 0xFFu);
                else PUTF("\x1b[38;2;%u;%u;%um", (cell->fg >> 16) & 0xFFu,
                          (cell->fg >> 8) & 0xFFu, cell->fg & 0xFFu);
            }
            if (cell->bg != VT_COLOR_DEFAULT) {
                if ((cell->bg & 0xFF000000u) == 0x01000000u) PUTF("\x1b[48;5;%um", cell->bg & 0xFFu);
                else PUTF("\x1b[48;2;%u;%u;%um", (cell->bg >> 16) & 0xFFu,
                          (cell->bg >> 8) & 0xFFu, cell->bg & 0xFFu);
            }
            attrs = a; fg = cell->fg; bg = cell->bg;
        }
        uint32_t cp = cell->cp ? cell->cp : ' ';
        if (cp < 0x80) {
            char ch = (char)cp;
            PUT(&ch, 1);
        } else if (cp < 0x800) {
            char u[2] = {(char)(0xc0 | (cp >> 6)), (char)(0x80 | (cp & 0x3f))};
            PUT(u, 2);
        } else if (cp < 0x10000) {
            char u[3] = {(char)(0xe0 | (cp >> 12)), (char)(0x80 | ((cp >> 6) & 0x3f)),
                         (char)(0x80 | (cp & 0x3f))};
            PUT(u, 3);
        } else {
            char u[4] = {(char)(0xf0 | (cp >> 18)), (char)(0x80 | ((cp >> 12) & 0x3f)),
                         (char)(0x80 | ((cp >> 6) & 0x3f)), (char)(0x80 | (cp & 0x3f))};
            PUT(u, 4);
        }
    }
    if (attrs || fg != VT_COLOR_DEFAULT || bg != VT_COLOR_DEFAULT) PUT("\x1b[0m", 4);
#undef PUT
#undef PUTF
    return o;
}

/* ---- disk log ---- */

static int session_dir(const char *name, char *out, size_t outsz) {
    /* The choke point: every path built from a session name goes through here,
     * from the daemon (sb_open) and from the client's `history` (sb_read_log)
     * alike. Callers validate too, so reaching this branch means a caller was
     * added without a gate — fail closed rather than mkdir outside the tree. */
    if (!at_valid_session_name(name)) { errno = EINVAL; return -1; }
    char base[512];
    if (at_sessions_dir(base, sizeof base) != 0) return -1;
    if ((size_t)snprintf(out, outsz, "%s/%s", base, name) >= outsz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir(out, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* Scan an existing log: returns highest seq + 1 (0 if empty/absent) and the
 * validated byte size. Truncates the file at the first corrupt record. */
static uint64_t scan_log(const char *path, uint64_t *valid_size) {
    *valid_size = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    uint64_t next = 0, off = 0;
    uint8_t hdr[16];
    static uint8_t payload[SB_LINE_MAX];
    for (;;) {
        ssize_t r = pread(fd, hdr, 16, (off_t)off);
        if (r < 16) break;
        uint32_t rec_len = get_u32(hdr);
        if (rec_len < 12 || rec_len - 12 > SB_LINE_MAX) break;
        uint32_t crc = get_u32(hdr + 4);
        uint64_t seq = get_u64(hdr + 8);
        uint32_t plen = rec_len - 12;
        if (pread(fd, payload, plen, (off_t)(off + 16)) != (ssize_t)plen) break;
        /* CRC covers line_seq + payload, hashed as one buffer. */
        static uint8_t scratch[8 + SB_LINE_MAX];
        put_u64(scratch, seq);
        memcpy(scratch + 8, payload, plen);
        if (crc32_buf(scratch, 8 + plen) != crc) break;
        next = seq + 1;
        off += 4 + rec_len;
    }
    close(fd);
    *valid_size = off;
    return next;
}

scrollback *sb_open(const char *session_name, uint32_t mem_lines, uint32_t file_max) {
    scrollback *sb = xcalloc(1, sizeof *sb);
    if (session_dir(session_name, sb->dir, sizeof sb->dir) != 0) {
        free(sb);
        return NULL;
    }
    snprintf(sb->log_path, sizeof sb->log_path, "%s/scrollback.log", sb->dir);
    sb->mem_lines = mem_lines ? mem_lines : SB_MEM_LINES_DEFAULT;
    sb->file_max = file_max ? file_max : SB_FILE_MAX_DEFAULT;
    sb->ring = xcalloc(sb->mem_lines, sizeof(mem_line));

    /* Resume seq numbering after the previous generation too. */
    char old_path[648];
    snprintf(old_path, sizeof old_path, "%s.1", sb->log_path);
    uint64_t old_size;
    uint64_t next_old = scan_log(old_path, &old_size);
    uint64_t valid;
    uint64_t next_cur = scan_log(sb->log_path, &valid);
    sb->next_seq = next_cur > next_old ? next_cur : next_old;

    sb->fd = open(sb->log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (sb->fd < 0) {
        free(sb->ring);
        free(sb);
        return NULL;
    }
    /* Drop any torn tail found by the scan. */
    struct stat st;
    if (fstat(sb->fd, &st) == 0 && (uint64_t)st.st_size > valid) {
        if (ftruncate(sb->fd, (off_t)valid) != 0) { /* keep going; append still safe */ }
    }
    sb->file_size = valid;
    return sb;
}

static void wbuf_drain(scrollback *sb) {
    size_t off = 0;
    while (off < sb->wbuf_len) {
        ssize_t w = write(sb->fd, sb->wbuf + off, sb->wbuf_len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            break; /* disk trouble: drop buffered lines, keep session alive */
        }
        off += (size_t)w;
    }
    sb->file_size += off;
    sb->wbuf_len = 0;
}

static void maybe_rotate(scrollback *sb) {
    /* Count coalesced-but-unwritten bytes too, or a large wbuf could defer
     * rotation far past the cap. */
    if (sb->file_size + sb->wbuf_len < sb->file_max) return;
    wbuf_drain(sb);
    fsync(sb->fd);
    close(sb->fd);
    char old_path[648];
    snprintf(old_path, sizeof old_path, "%s.1", sb->log_path);
    rename(sb->log_path, old_path); /* clobbers previous .1 */
    sb->fd = open(sb->log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    sb->file_size = 0;
}

void sb_push_line(scrollback *sb, const vt_cell *cells, uint16_t n) {
    if (!sb) return;
    static char text[SB_LINE_MAX];
    size_t len = serialize_line(cells, n, text, sizeof text);
    uint64_t seq = sb->next_seq++;

    /* ring */
    mem_line *slot = &sb->ring[(sb->ring_head + sb->ring_count) % sb->mem_lines];
    if (sb->ring_count == sb->mem_lines) {
        slot = &sb->ring[sb->ring_head];
        sb->ring_head = (sb->ring_head + 1) % sb->mem_lines;
        free(slot->text);
    } else {
        sb->ring_count++;
    }
    slot->text = xmalloc(len ? len : 1);
    memcpy(slot->text, text, len);
    slot->len = (uint32_t)len;
    slot->seq = seq;

    /* disk record */
    uint8_t hdr[16];
    uint32_t rec_len = 12 + (uint32_t)len;
    put_u32(hdr, rec_len);
    static uint8_t scratch[8 + SB_LINE_MAX];
    put_u64(scratch, seq);
    memcpy(scratch + 8, text, len);
    put_u32(hdr + 4, crc32_buf(scratch, 8 + len));
    put_u64(hdr + 8, seq);

    if (sb->wbuf_len + 16 + len > sizeof sb->wbuf) wbuf_drain(sb);
    memcpy(sb->wbuf + sb->wbuf_len, hdr, 16);
    memcpy(sb->wbuf + sb->wbuf_len + 16, text, len);
    sb->wbuf_len += 16 + len;

    maybe_rotate(sb);
}

uint64_t sb_total_lines(const scrollback *sb) { return sb ? sb->next_seq : 0; }

uint32_t sb_fetch(const scrollback *sb, uint64_t start_seq, uint32_t max_lines,
                  sb_line_ref *out, uint32_t out_cap) {
    if (!sb || sb->ring_count == 0) return 0;
    uint32_t filled = 0;
    for (uint32_t i = 0; i < sb->ring_count && filled < max_lines && filled < out_cap; i++) {
        const mem_line *l = &sb->ring[(sb->ring_head + i) % sb->mem_lines];
        if (l->seq < start_seq) continue;
        out[filled++] = (sb_line_ref){.text = l->text, .len = l->len, .seq = l->seq};
    }
    return filled;
}

void sb_flush(scrollback *sb) {
    if (sb) wbuf_drain(sb);
}

void sb_close(scrollback *sb) {
    if (!sb) return;
    wbuf_drain(sb);
    fsync(sb->fd);
    close(sb->fd);
    for (uint32_t i = 0; i < sb->ring_count; i++)
        free(sb->ring[(sb->ring_head + i) % sb->mem_lines].text);
    free(sb->ring);
    free(sb);
}

/* ---- offline log reading (history subcommand, dead sessions) ---- */

static int64_t read_one_log(const char *path, sb_read_cb cb, void *ud) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int64_t count = 0;
    uint64_t off = 0;
    uint8_t hdr[16];
    static uint8_t payload[SB_LINE_MAX];
    static uint8_t scratch[8 + SB_LINE_MAX];
    for (;;) {
        ssize_t r = pread(fd, hdr, 16, (off_t)off);
        if (r < 16) break;
        uint32_t rec_len = get_u32(hdr);
        if (rec_len < 12 || rec_len - 12 > SB_LINE_MAX) break;
        uint32_t crc = get_u32(hdr + 4);
        uint64_t seq = get_u64(hdr + 8);
        uint32_t plen = rec_len - 12;
        if (pread(fd, payload, plen, (off_t)(off + 16)) != (ssize_t)plen) break;
        put_u64(scratch, seq);
        memcpy(scratch + 8, payload, plen);
        if (crc32_buf(scratch, 8 + plen) != crc) break;
        cb(ud, seq, (const char *)payload, plen);
        count++;
        off += 4 + rec_len;
    }
    close(fd);
    return count;
}

int64_t sb_read_log(const char *session_name, sb_read_cb cb, void *ud) {
    char dir[600];
    if (session_dir(session_name, dir, sizeof dir) != 0) return -1;
    char path[664];
    int64_t total = -1;
    /* Older generation first so lines stream in order. */
    snprintf(path, sizeof path, "%s/scrollback.log.1", dir);
    int64_t a = read_one_log(path, cb, ud);
    snprintf(path, sizeof path, "%s/scrollback.log", dir);
    int64_t b = read_one_log(path, cb, ud);
    if (a >= 0 || b >= 0) total = (a > 0 ? a : 0) + (b > 0 ? b : 0);
    return total;
}

int sb_list_logs(char *buf, size_t bufsz) {
    char base[512];
    if (at_sessions_dir(base, sizeof base) != 0) return 0;
    DIR *d = opendir(base);
    if (!d) return 0;
    int count = 0;
    size_t off = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char log[1200];
        snprintf(log, sizeof log, "%s/%s/scrollback.log", base, e->d_name);
        struct stat st;
        char log1[1200];
        snprintf(log1, sizeof log1, "%s.1", log);
        if (stat(log, &st) != 0 && stat(log1, &st) != 0) continue;
        size_t nlen = strlen(e->d_name) + 1;
        if (off + nlen > bufsz) break;
        memcpy(buf + off, e->d_name, nlen);
        off += nlen;
        count++;
    }
    closedir(d);
    return count;
}
