#include "cuvs_ivfpq_format.h"

#include <stdio.h>
#include <sys/stat.h>

static int failures;

static void
check(const char *name, long got, long want)
{
    if (got != want)
    {
        fprintf(stderr, "FAIL %s: got %ld want %ld\n", name, got, want);
        failures++;
    }
}

static int
write_full(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len)
    {
        ssize_t n = write(fd, p, len);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int
write_npy_header(int fd, const char *descr, const char *shape)
{
    char dict[160], pad[128];
    unsigned char pre[10] = {0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0, 0, 0};
    int dlen = snprintf(dict, sizeof(dict),
        "{'descr': '%s', 'fortran_order': False, 'shape': %s}", descr, shape);
    size_t preamble = 10 + (size_t)dlen + 1;
    size_t padding = 64 - preamble % 64;
    uint16_t hlen = (uint16_t)(dlen + padding + 1);
    if (dlen < 0 || padding > sizeof(pad))
        return -1;
    pre[8] = (unsigned char)hlen;
    pre[9] = (unsigned char)(hlen >> 8);
    memset(pad, ' ', padding);
    if (write_full(fd, pre, sizeof(pre)) != 0 ||
        write_full(fd, dict, (size_t)dlen) != 0 ||
        write_full(fd, pad, padding) != 0 || write_full(fd, "\n", 1) != 0)
        return -1;
    return 0;
}

static int
write_scalar(int fd, const char *descr, uint64_t value, unsigned width)
{
    unsigned char raw[8] = {0};
    for (unsigned i = 0; i < width; i++)
        raw[i] = (unsigned char)(value >> (8 * i));
    return write_npy_header(fd, descr, "()") || write_full(fd, raw, width);
}

static int
write_array32(int fd, const uint32_t *values, uint64_t n)
{
    char shape[64];
    snprintf(shape, sizeof(shape), "(%llu,)", (unsigned long long)n);
    if (write_npy_header(fd, "<u4", shape) != 0)
        return -1;
    for (uint64_t i = 0; i < n; i++)
    {
        unsigned char raw[4] = {
            (unsigned char)values[i], (unsigned char)(values[i] >> 8),
            (unsigned char)(values[i] >> 16), (unsigned char)(values[i] >> 24)};
        if (write_full(fd, raw, 4) != 0)
            return -1;
    }
    return 0;
}

static int
write_fixture(const char *path, int conservative, uint32_t layout)
{
    uint32_t zero[2] = {0, 0};
    uint32_t sizes[2] = {1, 1025};
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    int bad =
        write_scalar(fd, "<u4", 4, 4) ||
        write_scalar(fd, "<u8", 1030, 8) ||
        write_scalar(fd, "<u8", 128, 8) ||
        write_scalar(fd, "<u4", 8, 4) ||
        write_scalar(fd, "<u4", 16, 4) ||
        write_scalar(fd, "|u1", (uint64_t)conservative, 1) ||
        write_scalar(fd, "<u4", 0, 4) ||
        write_scalar(fd, "<u4", 0, 4) ||
        write_scalar(fd, "<u4", layout, 4) ||
        write_scalar(fd, "<u4", 2, 4) ||
        write_array32(fd, zero, 1) ||
        write_array32(fd, zero, 1) ||
        write_array32(fd, zero, 1) ||
        write_array32(fd, zero, 1) ||
        write_array32(fd, sizes, 2);
    if (close(fd) != 0)
        bad = 1;
    return bad ? -1 : 0;
}

static uint64_t
file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (uint64_t)st.st_size : 0;
}

static void
run_case(const char *path, int conservative, uint32_t layout, uint64_t delta)
{
    CuvsIvfPqResidentEstimate estimate;
    int fd;
    if (write_fixture(path, conservative, layout) != 0 || (fd = open(path, O_RDONLY)) < 0)
    {
        fprintf(stderr, "FAIL fixture creation\n");
        failures++;
        return;
    }
    memset(&estimate, 0, sizeof(estimate));
    check("parse", cuvs_ivfpq_estimate_resident_fd(fd, file_size(path), &estimate), 0);
    check("padding", (long)estimate.padded_bytes, (long)delta);
    check("resident", (long)estimate.bytes, (long)(file_size(path) + delta));
    check("lists", estimate.n_lists, 2);
    check("fd offset unchanged", (long)lseek(fd, 0, SEEK_CUR), 0);
    close(fd);
}

int
main(int argc, char **argv)
{
    char path[] = "/tmp/pgcuvs-ivfpq-format-XXXXXX";
    if (argc == 2)
    {
        CuvsIvfPqResidentEstimate estimate;
        int fd = open(argv[1], O_RDONLY);
        if (fd < 0 || cuvs_ivfpq_estimate_resident_fd(fd, file_size(argv[1]), &estimate) != 0)
            return 2;
        printf("%llu\n", (unsigned long long)estimate.bytes);
        close(fd);
        return 0;
    }
    int fd = mkstemp(path);
    if (fd < 0)
        return 2;
    close(fd);

    /* Interleaved capacity: size 1 rounds its IDs to 32; size 1025 rounds to
     * 2048 with the non-conservative 1024-row list granularity. */
    run_case(path, 0, 1, 248 + 15872 + 8184);
    /* Conservative allocation rounds only to 32 rows, leaving just ID padding. */
    run_case(path, 1, 1, 248 + 248);
    /* Flat layout stores one code per row; capacity is the used size. */
    run_case(path, 0, 0, 0);

    unlink(path);
    if (failures)
        return 1;
    puts("test_ivfpq_resident_format: all checks passed");
    return 0;
}
