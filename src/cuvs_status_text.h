/*
 * cuvs_status_text.h — pure parsing for the I3-B text status ops
 * (CUVS_OP_INDEX_STATUS / MEMORY_STATUS / RECLAIM_STATUS).
 *
 * No PostgreSQL headers: pg_cuvs.c turns the parsed values into Datums, and
 * test/unit/test_status_text.c exercises every rule here on a laptop.
 *
 * Wire layout (cuvs_ipc.h I3-B block): '\n'-terminated lines, '\t'-separated
 * fields; the first line is the column header (*_COLS) and must match it
 * byte for byte, every following line has exactly *_NCOLS fields.
 */
#ifndef CUVS_STATUS_TEXT_H
#define CUVS_STATUS_TEXT_H

#include <stddef.h>
#include <stdint.h>

/* Field kinds, one char per column in header order. */
#define CUVS_SF_OID   'o'   /* uint32, never NULL */
#define CUVS_SF_INT   'i'   /* int32, never NULL */
#define CUVS_SF_GPU   'g'   /* int32, -1 -> NULL */
#define CUVS_SF_I64   'l'   /* int64, never NULL */
#define CUVS_SF_TIME  't'   /* epoch seconds >= 0, 0 -> NULL */
#define CUVS_SF_BOOL  'b'   /* 0 / 1 */
#define CUVS_SF_TRI   'r'   /* -1 -> NULL, else 0 / 1 */
#define CUVS_SF_TEXT  's'   /* any bytes, empty -> NULL */
#define CUVS_SF_I64N  'n'   /* I3-E: int64 >= 0, -1 -> NULL */

/* Kinds aligned with CUVS_INDEX_STATUS_COLS / CUVS_MEMORY_STATUS_COLS.
 * test_status_text.c asserts strlen == *_NCOLS. */
#define CUVS_INDEX_STATUS_KINDS  "oossglttlbltbsblsltsrl"
#define CUVS_MEMORY_STATUS_KINDS "illllllslstslllllllllllll"
/* I3-E kinds for *_COLS_I3E (appended after the I3-B kinds). */
#define CUVS_MEMORY_STATUS_KINDS_I3E "llnnlllllllllllstll"
#define CUVS_INDEX_STATUS_KINDS_I3E  "r"

/* Next line at *pos: returns 1 and sets *line (start) and *line_len (without
 * the '\n'), advancing *pos past the '\n'; 0 at the end of text; -1 when the
 * remaining bytes have no terminating '\n' (truncated reply). */
int cuvs_status_next_line(const char *text, size_t len, size_t *pos,
                          const char **line, size_t *line_len);

/* 1 when the first line equals header exactly (and is '\n'-terminated); then
 * *pos points at the first data line. 0 otherwise. */
int cuvs_status_check_header(const char *text, size_t len, const char *header,
                             size_t *pos);

/* Split line[0..line_len) in place into exactly n fields: each '\t' and the
 * byte at line[line_len] become NUL (the caller's buffer must have room for
 * that byte, e.g. the '\n' it replaces). Returns n, or -1 when the field
 * count differs or the line contains a NUL byte. */
int cuvs_status_split(char *line, size_t line_len, char **fields, int n);

/* Parse one NUL-terminated field of the given kind. Returns 0 and sets
 * *value / *isnull, or -1 when malformed (bad digits, out of range, bool not
 * 0/1, negative time). For CUVS_SF_TEXT *value is 0 and only *isnull is set. */
int cuvs_status_parse_field(char kind, const char *field, int64_t *value,
                            int *isnull);

/* Row visibility: privileged callers (superuser / pg_monitor) see every row;
 * others only rows of their own database whose index they own. */
int cuvs_status_row_visible(int is_priv, uint32_t row_db, uint32_t my_db,
                            int owns);

/* 1 when a manual-op failure text means the index is busy ("pinned..." or
 * "in use..."), which the backend reports as SQLSTATE 55006. */
int cuvs_status_msg_in_use(const char *msg);

/* RECLAIM_STATUS: when obj[0..len) is one JSON object "{...}" (surrounding
 * whitespace allowed) with a non-blank body, returns 1 and sets *body_off /
 * *body_len to the trimmed members between the braces. 0 otherwise. */
int cuvs_status_json_body(const char *obj, size_t len, size_t *body_off,
                          size_t *body_len);

#endif /* CUVS_STATUS_TEXT_H */
