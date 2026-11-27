#ifndef CUVS_IVFPQ_FORMAT_H
#define CUVS_IVFPQ_FORMAT_H

/* Minimal reader for the cuVS IVF-PQ serialization v4 fields that decide
 * resident list capacity. pread() is used throughout so the caller's FD offset
 * remains at zero for the real deserializer. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CUVS_IVFPQ_FORMAT_MAX_LISTS 65536u

typedef struct CuvsIvfPqResidentEstimate {
    uint64_t bytes;
    uint64_t padded_bytes;
    uint32_t n_lists;
    uint32_t pq_bits;
    uint32_t pq_dim;
    uint32_t codes_layout;
    int conservative;
} CuvsIvfPqResidentEstimate;

static int
cuvs_npy_pread(int fd, void *buf, size_t len, uint64_t *off)
{
    char *p = buf;
    size_t done = 0;
    while (done < len)
    {
        ssize_t n = pread(fd, p + done, len - done, (off_t)(*off + done));
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        done += (size_t)n;
    }
    *off += len;
    return 0;
}

static uint64_t
cuvs_npy_shape_count(const char *header)
{
    const char *shape = strstr(header, "'shape'");
    const char *open, *close;
    uint64_t count = 1;
    int saw = 0;
    if (!shape)
        return UINT64_MAX;
    open = strchr(shape, '(');
    close = open ? strchr(open, ')') : NULL;
    if (!open || !close)
        return UINT64_MAX;
    for (const char *p = open + 1; p < close; p++)
    {
        if (*p >= '0' && *p <= '9')
        {
            uint64_t v = 0;
            while (p < close && *p >= '0' && *p <= '9')
            {
                v = v * 10 + (uint64_t)(*p - '0');
                p++;
            }
            if (v && count > UINT64_MAX / v)
                return UINT64_MAX;
            count *= v;
            saw = 1;
        }
    }
    /* An empty shape "()" is a scalar and has one element. */
    return saw || close == open + 1 ? count : UINT64_MAX;
}

/* Open one NumPy v1 object. Return element count and byte width; *off is moved
 * to the start of its payload. */
static int
cuvs_npy_open(int fd, uint64_t *off, uint64_t *count, unsigned *itemsize,
              char *descr, size_t descr_len)
{
    unsigned char pre[10];
    uint16_t hlen;
    char *header, *d, *end;
    unsigned long width;
    if (cuvs_npy_pread(fd, pre, sizeof(pre), off) != 0 ||
        memcmp(pre, "\x93NUMPY", 6) != 0 || pre[6] != 1 || pre[7] != 0)
        return -1;
    hlen = (uint16_t)(pre[8] | ((uint16_t)pre[9] << 8));
    header = malloc((size_t)hlen + 1);
    if (!header)
        return -1;
    if (cuvs_npy_pread(fd, header, hlen, off) != 0)
    {
        free(header);
        return -1;
    }
    header[hlen] = '\0';
    d = strstr(header, "'descr'");
    if (!d || !(d = strchr(d, ':')) || !(d = strchr(d, '\'')))
    {
        free(header);
        return -1;
    }
    end = strchr(d + 1, '\'');
    if (!end || end - d - 1 < 2 || (size_t)(end - d - 1) >= descr_len)
    {
        free(header);
        return -1;
    }
    memcpy(descr, d + 1, (size_t)(end - d - 1));
    descr[end - d - 1] = '\0';
    width = strtoul(descr + 2, NULL, 10);
    if (!width)
    {
        free(header);
        return -1;
    }
    *itemsize = (unsigned)width;
    *count = cuvs_npy_shape_count(header);
    free(header);
    return *count == UINT64_MAX ? -1 : 0;
}

static int
cuvs_npy_scalar_u64(int fd, uint64_t *off, uint64_t *value)
{
    uint64_t count;
    unsigned width;
    char descr[16];
    unsigned char raw[8] = {0};
    if (cuvs_npy_open(fd, off, &count, &width, descr, sizeof(descr)) != 0 ||
        count != 1 || width == 0 || width > 8)
        return -1;
    if (cuvs_npy_pread(fd, raw, width, off) != 0)
        return -1;
    *value = 0;
    for (unsigned i = 0; i < width; i++)
        *value |= (uint64_t)raw[i] << (8 * i);
    return 0;
}

static int
cuvs_npy_skip_payload(int fd, uint64_t *off, uint64_t *count, unsigned *width)
{
    char descr[16];
    if (cuvs_npy_open(fd, off, count, width, descr, sizeof(descr)) != 0 ||
        (*count != 0 && *width > UINT64_MAX / *count))
        return -1;
    *off += *count * *width;
    return 0;
}

static uint64_t
cuvs_ivfpq_nextpow2(uint64_t n)
{
    uint64_t p = 1;
    while (p < n && p <= (UINT64_C(1) << 32))
        p <<= 1;
    return p;
}

static uint64_t
cuvs_ivfpq_capacity(uint64_t size, int conservative)
{
    uint64_t align_max = conservative ? 32 : 1024;
    if (size < align_max)
    {
        uint64_t floor = size < 32 ? 32 : size;
        uint64_t cap = cuvs_ivfpq_nextpow2(floor);
        return cap < align_max ? cap : align_max;
    }
    return ((size + align_max - 1) / align_max) * align_max;
}

/* Return the serialized artifact size plus exactly the extra bytes that
 * deserialize allocates for padded list capacity. The artifact's serialized
 * fixed arrays and valid-row payloads are used as the baseline; the adjustment
 * is only the capacity/used-size delta in codes and int64 source IDs. */
static int
cuvs_ivfpq_estimate_resident_fd(int fd, uint64_t file_bytes,
                                CuvsIvfPqResidentEstimate *out)
{
    uint64_t off = 0, version, rows, dim, bits, pqdim, cma, metric, book, layout, nlists;
    uint64_t count, delta = 0;
    unsigned width;
    uint32_t *sizes;
    if (cuvs_npy_scalar_u64(fd, &off, &version) != 0 || version != 4 ||
        cuvs_npy_scalar_u64(fd, &off, &rows) != 0 ||
        cuvs_npy_scalar_u64(fd, &off, &dim) != 0 ||
        cuvs_npy_scalar_u64(fd, &off, &bits) != 0 || bits == 0 || bits > 8 ||
        cuvs_npy_scalar_u64(fd, &off, &pqdim) != 0 || pqdim == 0 ||
        cuvs_npy_scalar_u64(fd, &off, &cma) != 0 ||
        cuvs_npy_scalar_u64(fd, &off, &metric) != 0 ||
        cuvs_npy_scalar_u64(fd, &off, &book) != 0 ||
        cuvs_npy_scalar_u64(fd, &off, &layout) != 0 ||
        cuvs_npy_scalar_u64(fd, &off, &nlists) != 0 ||
        nlists == 0 || nlists > CUVS_IVFPQ_FORMAT_MAX_LISTS)
        return -1;
    /* pq_centers, centers, centers_rot, rotation_matrix. */
    for (int i = 0; i < 4; i++)
        if (cuvs_npy_skip_payload(fd, &off, &count, &width) != 0)
            return -1;
    /* list_sizes: one uint32 per list. */
    {
        char descr[16];
        unsigned itemsize;
        if (cuvs_npy_open(fd, &off, &count, &itemsize, descr, sizeof(descr)) != 0 ||
            count != nlists || itemsize != 4)
            return -1;
    }
    sizes = malloc((size_t)nlists * sizeof(*sizes));
    if (!sizes)
        return -1;
    if (cuvs_npy_pread(fd, sizes, (size_t)nlists * sizeof(*sizes), &off) != 0)
    {
        free(sizes);
        return -1;
    }
    for (uint64_t i = 0; i < nlists; i++)
    {
        uint64_t size = sizes[i];
        uint64_t cap = layout == 0 ? size : cuvs_ivfpq_capacity(size, (int)cma);
        uint64_t before, after;
        if (layout == 0)      /* FLAT: one contiguous code per row. */
        {
            uint64_t code = (pqdim * bits + 7) / 8;
            before = size * code;
            after = cap * code;
        }
        else                  /* INTERLEAVED: 32-row groups of 16-byte chunks. */
        {
            uint64_t chunk = (16 * 8) / bits;
            uint64_t chunks = (pqdim + chunk - 1) / chunk;
            before = ((size + 31) / 32) * chunks * 32 * 16;
            after = ((cap + 31) / 32) * chunks * 32 * 16;
        }
        if (after < before || after - before > UINT64_MAX - delta)
        {
            free(sizes);
            return -1;
        }
        delta += after - before;
        if (cap < size || cap - size > (UINT64_MAX - delta) / 8)
        {
            free(sizes);
            return -1;
        }
        delta += (cap - size) * 8;
    }
    free(sizes);
    if (file_bytes > UINT64_MAX - delta)
        return -1;
    memset(out, 0, sizeof(*out));
    out->bytes = file_bytes + delta;
    out->padded_bytes = delta;
    out->n_lists = (uint32_t)nlists;
    out->pq_bits = (uint32_t)bits;
    out->pq_dim = (uint32_t)pqdim;
    out->codes_layout = (uint32_t)layout;
    out->conservative = (int)cma;
    return 0;
}

#endif
