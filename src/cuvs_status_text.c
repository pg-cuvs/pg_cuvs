/*
 * cuvs_status_text.c — pure parsing for the I3-B text status ops.
 * See cuvs_status_text.h. No PostgreSQL headers.
 */
#include "cuvs_status_text.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int
cuvs_status_next_line(const char *text, size_t len, size_t *pos,
                      const char **line, size_t *line_len)
{
    const char *start;
    const char *nl;

    if (*pos >= len)
        return 0;
    start = text + *pos;
    nl = memchr(start, '\n', len - *pos);
    if (nl == NULL)
        return -1;
    *line = start;
    *line_len = (size_t) (nl - start);
    *pos += *line_len + 1;
    return 1;
}

int
cuvs_status_check_header(const char *text, size_t len, const char *header,
                         size_t *pos)
{
    const char *line;
    size_t      line_len;
    size_t      p = 0;

    if (cuvs_status_next_line(text, len, &p, &line, &line_len) != 1)
        return 0;
    if (line_len != strlen(header) || memcmp(line, header, line_len) != 0)
        return 0;
    *pos = p;
    return 1;
}

int
cuvs_status_split(char *line, size_t line_len, char **fields, int n)
{
    int    count = 0;
    size_t i;

    if (n <= 0 || memchr(line, '\0', line_len) != NULL)
        return -1;
    fields[count++] = line;
    for (i = 0; i < line_len; i++)
    {
        if (line[i] != '\t')
            continue;
        if (count == n)
            return -1;
        line[i] = '\0';
        fields[count++] = line + i + 1;
    }
    line[line_len] = '\0';
    return count == n ? n : -1;
}

/* Strict decimal: optional '-', at least one digit, nothing after. */
static int
parse_i64(const char *s, int64_t *out)
{
    char      *end;
    long long  v;

    if (s[0] == '\0' || isspace((unsigned char) s[0]) || s[0] == '+')
        return -1;
    errno = 0;
    v = strtoll(s, &end, 10);
    if (errno != 0 || *end != '\0' || end == s)
        return -1;
    *out = (int64_t) v;
    return 0;
}

int
cuvs_status_parse_field(char kind, const char *field, int64_t *value,
                        int *isnull)
{
    int64_t v = 0;

    *value = 0;
    *isnull = 0;
    if (kind == CUVS_SF_TEXT)
    {
        *isnull = field[0] == '\0';
        return 0;
    }
    if (parse_i64(field, &v) != 0)
        return -1;
    switch (kind)
    {
        case CUVS_SF_OID:
            if (v < 0 || v > (int64_t) UINT32_MAX)
                return -1;
            break;
        case CUVS_SF_INT:
            if (v < INT32_MIN || v > INT32_MAX)
                return -1;
            break;
        case CUVS_SF_GPU:
            if (v < INT32_MIN || v > INT32_MAX)
                return -1;
            *isnull = v == -1;
            break;
        case CUVS_SF_I64:
            break;
        case CUVS_SF_TIME:
            if (v < 0)
                return -1;
            *isnull = v == 0;
            break;
        case CUVS_SF_BOOL:
            if (v != 0 && v != 1)
                return -1;
            break;
        case CUVS_SF_TRI:
            if (v != -1 && v != 0 && v != 1)
                return -1;
            *isnull = v == -1;
            break;
        default:
            return -1;
    }
    *value = *isnull ? 0 : v;
    return 0;
}

int
cuvs_status_row_visible(int is_priv, uint32_t row_db, uint32_t my_db, int owns)
{
    return is_priv || (row_db == my_db && owns);
}

int
cuvs_status_msg_in_use(const char *msg)
{
    return strncmp(msg, "pinned", 6) == 0 || strncmp(msg, "in use", 6) == 0;
}

int
cuvs_status_json_body(const char *obj, size_t len, size_t *body_off,
                      size_t *body_len)
{
    size_t b = 0;
    size_t e = len;

    while (b < e && isspace((unsigned char) obj[b]))
        b++;
    while (e > b && isspace((unsigned char) obj[e - 1]))
        e--;
    if (e - b < 2 || obj[b] != '{' || obj[e - 1] != '}')
        return 0;
    b++;
    e--;
    while (b < e && isspace((unsigned char) obj[b]))
        b++;
    while (e > b && isspace((unsigned char) obj[e - 1]))
        e--;
    if (b == e || memchr(obj + b, '\0', e - b) != NULL)
        return 0;
    *body_off = b;
    *body_len = e - b;
    return 1;
}
