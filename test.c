#include <pthread.h>

#include "xmalloc.h"

#define THREAD_COUNT 5

void *worker(void *args) {
    xmalloc(100);
    return NULL;
}

int main(void) {
    pthread_t threads[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_create(&threads[i], NULL, worker, NULL);
    }
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }
    return 0;
}
