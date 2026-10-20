#define _GNU_SOURCE 1
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

#include "cuvs_hnsw_build_common.h"
#include <assert.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#ifdef __linux__
#include <sys/mman.h>
#endif
int main(void) {
    CuvsHnswCapacity c;
    assert(cuvs_hnsw_metric_from_family("halfvec_l2_ops") == CUVS_METRIC_L2);
    assert(cuvs_hnsw_metric_from_family("halfvec_cosine_ops") == CUVS_METRIC_COSINE);
    assert(cuvs_hnsw_metric_from_family("halfvec_ip_ops") == CUVS_METRIC_IP);
    assert(cuvs_hnsw_metric_from_family("vector_l2_ops") == CUVS_METRIC_L2);
    assert(cuvs_hnsw_metric_from_family("vector_cosine_ops") == CUVS_METRIC_COSINE);
    assert(cuvs_hnsw_metric_from_family("vector_ip_ops") == CUVS_METRIC_IP);
    assert(cuvs_hnsw_metric_from_family("halfvec_cosine_ops_extra") == -1);
    assert(cuvs_hnsw_metric_from_family(NULL) == -1);
    int p[2];
    char path[] = "/tmp/pgcuvs-core-unit-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0); unlink(path);
    assert(cuvs_hnsw_regular_disk(fd));
#ifdef __linux__
    int ram_fd = memfd_create("hnsw-unit-not-disk", MFD_CLOEXEC);
    assert(ram_fd >= 0 && !cuvs_hnsw_regular_disk(ram_fd));
    close(ram_fd);
#endif
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, p) == 0);
    assert(!cuvs_hnsw_regular_disk(p[0]));
    assert(!cuvs_hnsw_owner_cancelled(p[0]));
    assert(write(p[1], "C", 1) == 1);
    assert(cuvs_hnsw_owner_cancelled(p[0]));
    close(p[0]); close(p[1]);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, p) == 0);
    close(p[1]); assert(cuvs_hnsw_owner_cancelled(p[0])); close(p[0]);
    assert(cuvs_hnsw_capacity(10000, 128, 64, 128, 0, &c) == 0);
    assert(c.raw_bytes == 5120000 && c.serializer_floor >= 200000);
    assert(c.host_peak > c.raw_bytes && c.gpu_peak > 0);
    assert(cuvs_hnsw_capacity(UINT64_MAX, 128, 64, 128, 0, &c) != 0);
    assert(cuvs_hnsw_capacity(100, 0, 64, 128, 0, &c) != 0);
    assert(cuvs_hnsw_capacity(100, 128, 65, 128, 0, &c) != 0);
    assert(cuvs_hnsw_ace_partitions(10000000, 1024, 64, 128,
        8ULL << 30, 8ULL << 30) > 1);
    assert(cuvs_hnsw_ace_partitions(10000000, 1024, 64, 128,
        128ULL << 20, 8ULL << 30) == 0);
    // Arrange a serialized one-row graph with counted self padding.
    uint64_t header[12] = {0};
    uint32_t padded[4] = {0, 0, 0, 0}, count = 4;
    uint64_t label = 0;
    header[1] = header[2] = 1; header[3] = 36;
    header[4] = 28; header[5] = 20; header[7] = header[9] = 2; header[8] = 4;
    assert(pwrite(fd, header, 96, 0) == 96);
    assert(pwrite(fd, &count, 4, 96) == 4);
    assert(pwrite(fd, padded, 16, 100) == 16);
    assert(pwrite(fd, &label, 8, 124) == 8);
    // Act: normalize only the bounded tiny graph's counted base links.
    assert(cuvs_hnsw_fix_tiny_counts(fd, 1) == 0);
    // Assert: padding is not a searchable neighbor; malformed IDs fail closed.
    assert(pread(fd, &count, 4, 96) == 4 && count == 0);
    // Two-row graphs retain one distinct non-self neighbor, not duplicate padding.
    header[1] = header[2] = 2;
    assert(pwrite(fd, header, 96, 0) == 96);
    for (uint64_t row = 0; row < 2; row++) {
        count = 4; label = row;
        padded[0] = padded[1] = (uint32_t)(1 - row);
        padded[2] = padded[3] = (uint32_t)row;
        assert(pwrite(fd, &count, 4, 96 + row * 36) == 4);
        assert(pwrite(fd, padded, 16, 100 + row * 36) == 16);
        assert(pwrite(fd, &label, 8, 124 + row * 36) == 8);
    }
    assert(cuvs_hnsw_fix_tiny_counts(fd, 2) == 0);
    for (uint64_t row = 0; row < 2; row++) {
        assert(pread(fd, &count, 4, 96 + row * 36) == 4 && count == 1);
        assert(pread(fd, padded, 16, 100 + row * 36) == 16 && padded[0] == 1 - row);
    }
    count = 1; padded[0] = 2;
    assert(pwrite(fd, &count, 4, 96) == 4);
    assert(pwrite(fd, padded, 16, 100) == 16);
    assert(cuvs_hnsw_fix_tiny_counts(fd, 2) != 0);
    close(fd);
    puts("PASS capacity/ACE, disk FD, cancel/owner EOF, tiny distinct non-self counts and malformed-ID rejection");
}
