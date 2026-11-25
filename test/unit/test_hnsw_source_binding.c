/* A source binding survives rollback of its bytes, and rejects changed bytes. */
#define _POSIX_C_SOURCE 200809L
#include "cuvs_hnsw_source.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#define TEST_ATIME st_atimespec
#define TEST_MTIME st_mtimespec
#define TEST_CTIME st_ctimespec
#else
#define TEST_ATIME st_atim
#define TEST_MTIME st_mtim
#define TEST_CTIME st_ctim
#endif

typedef struct BindingFixture {
    char dir[64];
    char graph[96];
    char tids[96];
    char backup[96];
    char replacement[96];
    FILE *record;
} BindingFixture;

static const char tids_bytes[] = "old TIDs";

static void
write_file(const char *path, const char *bytes)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    assert(write(fd, bytes, strlen(bytes)) == (ssize_t) strlen(bytes));
    assert(close(fd) == 0);
}

static bool
matches(const BindingFixture *fixture, bool (*cancel_pending)(void))
{
    CuvsHnswSourceBinding observed;
    int graph = open(fixture->graph, O_RDONLY);
    int tids = open(fixture->tids, O_RDONLY);

    assert(graph >= 0 && tids >= 0);
    rewind(fixture->record);
    bool valid = cuvs_hnsw_source_binding_read(fixture->record, graph, tids,
                                               &observed, cancel_pending);
    assert(close(graph) == 0);
    assert(close(tids) == 0);
    return valid;
}

static void
setup(BindingFixture *fixture)
{
    CuvsHnswSourceBinding stored = {.magic = CUVS_HNSW_SOURCE_MAGIC,
                                   .version = CUVS_HNSW_SOURCE_VERSION};

    strcpy(fixture->dir, "/tmp/cuvs-source-binding-XXXXXX");
    assert(mkdtemp(fixture->dir) != NULL);
    snprintf(fixture->graph, sizeof(fixture->graph), "%s/source.hnsw", fixture->dir);
    snprintf(fixture->tids, sizeof(fixture->tids), "%s/source.tids", fixture->dir);
    snprintf(fixture->backup, sizeof(fixture->backup), "%s/backup.tids", fixture->dir);
    snprintf(fixture->replacement, sizeof(fixture->replacement), "%s/new.tids", fixture->dir);
    write_file(fixture->graph, "old graph");
    write_file(fixture->tids, tids_bytes);

    int graph = open(fixture->graph, O_RDONLY);
    int tids = open(fixture->tids, O_RDONLY);
    assert(graph >= 0 && tids >= 0);
    assert(cuvs_hnsw_file_identity(graph, &stored.graph) == 0);
    assert(cuvs_hnsw_file_identity(tids, &stored.tids) == 0);
    assert(close(graph) == 0);
    assert(close(tids) == 0);

    fixture->record = tmpfile();
    assert(fixture->record != NULL);
    assert(fwrite(&stored, sizeof(stored), 1, fixture->record) == 1);
    assert(matches(fixture, NULL));
}

static void
cleanup(BindingFixture *fixture)
{
    assert(fclose(fixture->record) == 0);
    assert(unlink(fixture->graph) == 0);
    assert(unlink(fixture->tids) == 0);
    assert(rmdir(fixture->dir) == 0);
}

static void
assert_original_metadata(const struct stat *before, const struct stat *after)
{
    assert(before->st_dev == after->st_dev && before->st_ino == after->st_ino &&
           before->st_size == after->st_size);
    assert(before->TEST_MTIME.tv_sec == after->TEST_MTIME.tv_sec &&
           before->TEST_MTIME.tv_nsec == after->TEST_MTIME.tv_nsec);
}

static bool
rollback(bool replaced)
{
    BindingFixture fixture;
    struct stat before, after;
    const struct timespec pause = {.tv_nsec = 20000000};
    char contents[sizeof(tids_bytes)] = {0};

    setup(&fixture);
    assert(stat(fixture.tids, &before) == 0);
    assert(nanosleep(&pause, NULL) == 0);
    assert(linkat(AT_FDCWD, fixture.tids, AT_FDCWD, fixture.backup, 0) == 0);
    if (replaced)
    {
        write_file(fixture.replacement, "new TIDs");
        assert(rename(fixture.replacement, fixture.tids) == 0);
        assert(rename(fixture.backup, fixture.tids) == 0);
    }
    else
        assert(unlink(fixture.backup) == 0);

    int fd = open(fixture.tids, O_RDONLY);
    assert(fd >= 0);
    assert(fstat(fd, &after) == 0);
    assert_original_metadata(&before, &after);
    assert(before.TEST_CTIME.tv_sec != after.TEST_CTIME.tv_sec ||
           before.TEST_CTIME.tv_nsec != after.TEST_CTIME.tv_nsec);
    assert(read(fd, contents, sizeof(contents)) == (ssize_t) strlen(tids_bytes));
    assert(strcmp(contents, tids_bytes) == 0);
    assert(close(fd) == 0);

    bool valid = matches(&fixture, NULL);
    printf("%s: %s\n", replaced ? "replacement rolled back" : "backup discarded",
           valid ? "PASS" : "FAIL (binding rejected)");
    cleanup(&fixture);
    return valid;
}

static bool
mutation(bool graph)
{
    BindingFixture fixture;
    struct stat before, after;
    setup(&fixture);
    const char *path = graph ? fixture.graph : fixture.tids;
    int fd = open(path, O_RDWR);
    assert(fd >= 0);
    assert(fstat(fd, &before) == 0);
    const struct timespec times[2] = {before.TEST_ATIME, before.TEST_MTIME};
    assert(pwrite(fd, "X", 1, 0) == 1);
    assert(futimens(fd, times) == 0);
    assert(fstat(fd, &after) == 0);
    assert_original_metadata(&before, &after);
    assert(close(fd) == 0);

    bool valid = !matches(&fixture, NULL);
    printf("changed %s with original inode, length and mtime: %s\n",
           graph ? "graph" : "TIDs", valid ? "PASS" : "FAIL (binding accepted)");
    cleanup(&fixture);
    return valid;
}

static bool
invalidation(void)
{
    BindingFixture fixture;
    const uint32_t zero = 0;
    setup(&fixture);
    assert(pwrite(fileno(fixture.record), &zero, sizeof(zero), 0) == sizeof(zero));
    bool valid = !matches(&fixture, NULL);
    printf("invalidated binding for unchanged files: %s\n", valid ? "PASS" : "FAIL");
    cleanup(&fixture);
    return valid;
}

static FILE *binding_during_read;

static bool
invalidate_during_hash(void)
{
    if (binding_during_read)
    {
        const uint32_t zero = 0;
        assert(pwrite(fileno(binding_during_read), &zero, sizeof(zero), 0) == sizeof(zero));
        binding_during_read = NULL;
    }
    return false;
}

static bool
invalidation_during_hash(void)
{
    BindingFixture fixture;
    setup(&fixture);
    binding_during_read = fixture.record;
    bool valid = !matches(&fixture, invalidate_during_hash);
    assert(binding_during_read == NULL);
    printf("invalidation during hashing: %s\n", valid ? "PASS" : "FAIL");
    cleanup(&fixture);
    return valid;
}

int
main(void)
{
    unsigned int failures = 0;
    failures += !rollback(false);
    failures += !rollback(true);
    failures += !mutation(false);
    failures += !mutation(true);
    failures += !invalidation();
    failures += !invalidation_during_hash();
    return failures != 0;
}
