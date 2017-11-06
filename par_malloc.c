#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <pthread.h>

#include "xmalloc.h"


#define PAGE_SIZE_BYTES 4096
#define INIT_PAGE_ALLOC 1024

#define ROUND_UP_TO_PAGE(x) ( (x + (PAGE_SIZE_BYTES - 1)) & (~(PAGE_SIZE_BYTES - 1)))

#define SIZE_CLASS_0 16
#define SIZE_CLASS_1 32
#define SIZE_CLASS_2 64
#define SIZE_CLASS_3 512
#define SIZE_CLASS_4 1024
#define SIZE_CLASS_5 2048

#define THREAD_CACHE_ALLOC_MAX SIZE_CLASS_5


typedef struct page_stack_node page_stack_node;
struct page_stack_node {
    page_stack_node *next;
};

typedef struct block_header block_header;
struct block_header {
    union {
        void *next;
        size_t size;
    } h;
};
const size_t BLOCK_HEADER_SIZE = sizeof(block_header);


int _allocator_initialized = -1;
pthread_once_t _init_once_control = PTHREAD_ONCE_INIT;

// These are only used to lock the global free page stack which should ideally be minimally contested
pthread_mutex_t global_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_spinlock_t global_spinlock;

page_stack_node *global_page_stack = NULL;


// Not thread safe
static size_t _free_page_stack_size_unsafe(void) {
    size_t size = 0;
    page_stack_node *node = global_page_stack;
    while (node) {
        size++;
        node = node->next;
    }
    return size;
}


// Allocate a new batch of pages for the free page stack
// This is really expensive which is why we generally choose to allocate so many at once
static int _alloc_system_pages_unsafe(void) {
    char *init_pages = mmap(NULL, PAGE_SIZE_BYTES * INIT_PAGE_ALLOC, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (!init_pages) {
        // We're fucked
        return -1;
    }
    
    char *current_page = init_pages;
    for (int page = 0; page < INIT_PAGE_ALLOC - 1; page++) {
        page_stack_node *node = (page_stack_node *) current_page;
        node->next = (page_stack_node *) (current_page + PAGE_SIZE_BYTES);
        current_page += PAGE_SIZE_BYTES;
    }
    ((page_stack_node *) current_page)->next = NULL;
    global_page_stack = (page_stack_node *) init_pages;

    assert(_free_page_stack_size_unsafe() == INIT_PAGE_ALLOC);
    return 0;
}


static void *_get_free_page_unsafe() {
    if (!global_page_stack) {
        _alloc_system_pages_unsafe();
    }
    void *page = (void *) global_page_stack;
    global_page_stack = global_page_stack->next;
    return page;
}


// Grab a page from the central free page stack
static void *get_free_page(void) {
    pthread_spin_lock(&global_spinlock);
    void *page = _get_free_page_unsafe();
    pthread_spin_unlock(&global_spinlock);
    return page;
}


static void xmalloc_init(void) {
    pthread_mutex_lock(&global_mutex);
    (void) _alloc_system_pages_unsafe();
    pthread_spin_init(&global_spinlock, PTHREAD_PROCESS_PRIVATE);
    _allocator_initialized = 1;
    pthread_mutex_unlock(&global_mutex);
}


static inline void _xmalloc_check_init(void) {
    if (_allocator_initialized < 0) {
        _allocator_initialized = 0;
        pthread_once(&_init_once_control, xmalloc_init);
    }
    while (_allocator_initialized <= 0) {}
}

void *xmalloc(size_t size) { 
    _xmalloc_check_init();
    size += BLOCK_HEADER_SIZE;
    if (size > THREAD_CACHE_ALLOC_MAX) {
        // Map pages directly. Expensive but rare
        size = ROUND_UP_TO_PAGE(size);
        void *alloc = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (alloc == MAP_FAILED) {
            return NULL;
        }
        block_header *header = (block_header *) alloc;
        header->h.size = size;
        return (void *) (header + 1);
    } else {

    }
    return NULL;
}


void xfree(void *ptr) {

}


void *xrealloc(void *prev, size_t bytes) {
    return 0;
}
