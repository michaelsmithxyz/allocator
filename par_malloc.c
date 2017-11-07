#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <syscall.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <pthread.h>

#include "xmalloc.h"


#define PAGE_SIZE_BYTES 4096
#define INIT_PAGE_ALLOC 1024

#define ROUND_UP_TO_PAGE(x) ( (x + (PAGE_SIZE_BYTES - 1)) & (~(PAGE_SIZE_BYTES - 1)))


#define SIZE_CLASS_0 32
#define SIZE_CLASS_1 64
#define SIZE_CLASS_2 128
#define SIZE_CLASS_3 256
#define SIZE_CLASS_4 512
#define SIZE_CLASS_5 1024
#define SIZE_CLASS_6 2048

#define SIZE_CLASS_TO_SIZE(n) (1 << (5 + n))

#define SIZE_CLASS_NUM 7
#define THREAD_CACHE_ALLOC_MAX SIZE_CLASS_5


typedef struct page_stack_node page_stack_node;
struct page_stack_node {
    page_stack_node *next;
};

typedef struct bin_stack_node bin_stack_node;
struct bin_stack_node {
    bin_stack_node *next;
};

typedef struct thread_cache_bins thread_cache_bins;
struct thread_cache_bins {
    bin_stack_node *bins[SIZE_CLASS_NUM];
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

// This is only used to lock the global free page stack which should ideally be minimally contested
pthread_spinlock_t global_spinlock;

// Global page stack (needs to be locked)
page_stack_node *global_page_stack = NULL;

// Thread local thread cache (free list bins)
__thread thread_cache_bins *thread_cache = NULL;
__thread pid_t current_thread_id = 0;
__thread int current_thread_initialized = 0;


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


// Initialize global allocator state
static void xmalloc_init(void) {
    (void) _alloc_system_pages_unsafe();
    pthread_spin_init(&global_spinlock, PTHREAD_PROCESS_PRIVATE);
    _allocator_initialized = 1;
}


static inline size_t best_size_class(size_t size) {
    // TODO: Optimize this with bit tricks
    if (size <= 32) {
        return 0;
    } else if (size <= 64) {
        return 1;
    } else if (size <= 128) {
        return 2;
    } else if (size <= 256) {
        return 3;
    } else if (size <= 512) {
        return 4;
    } else if (size <= 1024) {
        return 5;
    }
    return 6;
}


static void replenish_thread_cache(void) {
    for (size_t i = 0; i < SIZE_CLASS_NUM; i++) {
        if (!thread_cache->bins[i]) {
            void *page = get_free_page();
            size_t bin_size = SIZE_CLASS_TO_SIZE(i);
            size_t cells = PAGE_SIZE_BYTES / bin_size;
            
            char *current_cell = (char *) page;
            for (size_t cell_num = 0; cell_num < cells - 1; cell_num++) {
                bin_stack_node *node = (bin_stack_node *) current_cell;
                node->next = (bin_stack_node *) (current_cell + bin_size);
                current_cell = current_cell + bin_size;
            }
            ((bin_stack_node *) current_cell)->next = NULL;
            thread_cache->bins[i] = (bin_stack_node *) page;
        }
    }
}


static inline void _init_thread_cache(void) {
    thread_cache_bins *cache = get_free_page();
    for (size_t i = 0; i < SIZE_CLASS_NUM; i++) {
        cache->bins[i] = NULL;
    }
    thread_cache = cache;
}


// Initialize thread-local state
static void xmalloc_thread_init(void) {
    current_thread_id = syscall(SYS_gettid);
    _init_thread_cache();
    replenish_thread_cache();
    current_thread_initialized = 1;
}


static inline void _xmalloc_check_init(void) {
    if (_allocator_initialized < 0) {
        _allocator_initialized = 0;
        pthread_once(&_init_once_control, xmalloc_init);
    }
    while (_allocator_initialized <= 0) {}
    if (!current_thread_initialized) {
        xmalloc_thread_init();
    }
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
