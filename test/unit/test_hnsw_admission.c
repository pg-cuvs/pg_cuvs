#define _GNU_SOURCE 1
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "cuvs_hnsw_build_common.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>

typedef struct {
    pthread_mutex_t mutex;
    int owner;
    int busy;
    atomic_int observed;
    atomic_int admitted;
    atomic_int finished;
    int rc;
} Fixture;

static int gpu_busy(void *arg) {
    Fixture *f = arg;
    atomic_store(&f->observed, 1);
    return f->busy;
}
static void *waiter(void *arg) {
    Fixture *f = arg;
    f->rc = cuvs_hnsw_wait_for_idle(&f->mutex, f->owner, gpu_busy, f);
    if (f->rc == 0) {
        atomic_store(&f->admitted, 1);
        pthread_mutex_unlock(&f->mutex);
    }
    atomic_store(&f->finished, 1);
    return NULL;
}
static void run_case(int cancel) {
    int pair[2];
    pthread_t thread;
    Fixture f = {.mutex=PTHREAD_MUTEX_INITIALIZER, .busy=1};
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    f.owner = pair[0];
    // Arrange: a prior unlocked GPU operation remains registered as busy.
    assert(pthread_create(&thread, NULL, waiter, &f) == 0);
    for (int i=0; !atomic_load(&f.observed) && i<1000; i++) poll(NULL, 0, 1);
    assert(atomic_load(&f.observed) && !atomic_load(&f.admitted));
    // Act: completion must be able to reacquire the registry mutex; cancellation
    // must also return while the prior operation is still busy.
    int lock_rc = 1;
    for (int i=0; lock_rc && i<1000; i++) {
        lock_rc = pthread_mutex_trylock(&f.mutex);
        if (lock_rc) poll(NULL, 0, 1);
    }
    assert(lock_rc == 0);
    if (cancel) assert(write(pair[1], "C", 1) == 1);
    else f.busy = 0;
    pthread_mutex_unlock(&f.mutex);
    for (int i=0; !atomic_load(&f.finished) && i<1000; i++) poll(NULL, 0, 1);
    assert(atomic_load(&f.finished));
    assert(pthread_join(thread, NULL) == 0);
    // Assert: no admission across a live GPU scope, and no mutex left held.
    assert(cancel ? f.rc != 0 && !atomic_load(&f.admitted) : f.rc == 0 && atomic_load(&f.admitted));
    assert(pthread_mutex_trylock(&f.mutex) == 0);
    pthread_mutex_unlock(&f.mutex);
    pthread_mutex_destroy(&f.mutex);
    close(pair[0]); close(pair[1]);
}
int main(void) {
    run_case(0); run_case(1);
    puts("PASS HNSW drains prior GPU work without holding registry lock; wait is owner-cancellable");
}
