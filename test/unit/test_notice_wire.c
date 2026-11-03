#include "cuvs_util.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int main(void)
{
    CuvsNoticeBatch batch;
    char wire[CUVS_NOTICE_OUT_BYTES] = "";
    const char *messages[] = {"self replacement", "managed spill", "deferred reload"};
    const char *details[] = {"old copy unloaded", "may be slower", "saved on disk"};

    assert(CUVS_NOTICE_OUT_BYTES == 1024);
    for (int i = 0; i < 3; i++)
        assert(cuvs_notice_append(wire, sizeof(wire), messages[i], details[i]) == 0);
    assert(cuvs_notice_decode(wire, sizeof(wire), &batch) == 0 && batch.count == 3);
    for (int i = 0; i < 3; i++)
    {
        assert(strcmp(batch.pairs[i].message, messages[i]) == 0);
        assert(strcmp(batch.pairs[i].detail, details[i]) == 0);
    }
    assert(cuvs_notice_append(wire, sizeof(wire), "fourth", "full journal") == 1);
    assert(strcmp(wire, "(+4 more)") == 0);
    assert(cuvs_notice_append(wire, sizeof(wire), "fifth", "full journal") == 1);
    assert(strcmp(wire, "(+5 more)") == 0);

    /* Accessible input sizes 1023/1024/1025, with NUL at the final byte. */
    for (size_t n = 1023; n <= 1025; n++)
    {
        char *input = malloc(n);
        assert(input);
        memset(input, 'x', n);
        input[n - 1] = '\0';
        int rc = cuvs_notice_decode(input, n, &batch);
        assert((rc == 0) == (n <= 1024));
        free(input);
    }
    /* Even a claimed larger length never reads beyond the wire field. */
    char *guarded = malloc(1024);
    assert(guarded);
    memset(guarded, 'x', 1024);
    assert(cuvs_notice_decode(guarded, 1025, &batch) == -1);
    free(guarded);
    assert(cuvs_notice_decode("N1:", 4, &batch) == -1);
    assert(cuvs_notice_decode("N1:a\037b\036c\037d\036e\037f\036g\037h",
           sizeof("N1:a\037b\036c\037d\036e\037f\036g\037h"), &batch) == -1);
    memset(wire, 0, sizeof(wire));
    assert(cuvs_notice_append(wire, sizeof(wire), "bad\036record", "") == -1);
    char large[1025];
    memset(large, 'q', sizeof(large));
    large[1024] = 0;
    assert(cuvs_notice_append(wire, sizeof(wire), large, "") == 1);
    assert(strcmp(wire, "(+1 more)") == 0);
    puts("PASS notice3 pairs,1023/1024/1025 boundaries,overflow count,malformed records");
    return 0;
}
