#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>

#include "hmalloc.h"


const size_t PAGE_SIZE = 4096;

typedef struct free_block_header free_block_header;
struct free_block_header {
    size_t size; // Size includes the header itself
    free_block_header *next; 
};

typedef struct alloc_block_header alloc_block_header;
struct alloc_block_header {
    size_t size; // Size includes the header itself
};


static hm_stats stats;
static free_block_header *free_list = NULL;

// mutex
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
int count;

static long free_list_length() {
    long length = 0;
    free_block_header *node = free_list;
    while (node) {
        length++;
        node = node->next;
    }
    return length;
}

hm_stats *hgetstats() {
    stats.free_length = free_list_length();
    return &stats;
}

void hprintstats() {
    hm_stats *hstats = hgetstats();
    fprintf(stderr, "\n== husky malloc stats ==\n");
    fprintf(stderr, "Mapped:   %ld\n", hstats->pages_mapped);
    fprintf(stderr, "Unmapped: %ld\n", hstats->pages_unmapped);
    fprintf(stderr, "Allocs:   %ld\n", hstats->chunks_allocated);
    fprintf(stderr, "Frees:    %ld\n", hstats->chunks_freed);
    fprintf(stderr, "Freelen:  %ld\n", hstats->free_length);
}

static size_t ceil_div(size_t xx, size_t yy) {
    size_t zz = xx / yy;
    if (zz * yy == xx) {
        return zz;
    } else {
        return zz + 1;
    }
}


static void coalesce_free_list() {
    free_block_header *current = free_list;
    while (current && current->next) {
        // Try to merge current with the next one
        if ((void *) (((char *) current) + current->size) == (void *) current->next) {
            // We can merge!
            current->size = current->size + current->next->size;
            current->next = current->next->next;
            continue;
        }
        current = current->next;
    }
}

// Insert a free block onto the list and coalesce blocks
static void insert_free_block(free_block_header *block) {
    if (!free_list) {
        block->next = NULL;
        free_list = block;
    } else {
        if (free_list > block) {
            // Insert block at the beginning
            block->next = free_list;
            free_list = block;
        } else {
            // Otherwise find a spot for it
            free_block_header *current = free_list;
            while (current->next) {
                if (current->next > block) {
                    break;
                }
                current = current->next;
            }
            block->next = current->next;
            current->next = block;
        } 
    }
    coalesce_free_list();
}

// Find and remove a free block >= size from the free list
static free_block_header *get_free_block(size_t size) {
    if (free_list) {
        if (free_list->size >= size) {
            // If the first element of the free list is big enough
            free_block_header *block = free_list;
            free_list = free_list->next;
            size_t rest = block->size - size;
            if (rest >= sizeof(free_block_header)) {
                free_block_header *new_free_block = (free_block_header *) (((char *) block) + size);
                new_free_block->size = rest;
                insert_free_block(new_free_block);
                block->size = size;
            }
            return block;
        }
        free_block_header *current = free_list;
        while (current->next) {
            if (current->next->size >= size) {
                free_block_header *block = current->next;
                current->next = current->next->next;

                size_t rest = block->size - size;
                if (rest >= sizeof(free_block_header)) {
                    free_block_header *new_free_block = (free_block_header *) (((char *) block) + size);
                    new_free_block->size = rest;
                    insert_free_block(new_free_block);
                    block->size = size;
                }
                return block;
            }
            current = current->next;
        }
    }
    return NULL;
}

void *hrealloc(void *ptr, size_t size) {
    alloc_block_header *old_alloc = ((alloc_block_header *) ptr) - 1;
    if (old_alloc->size < size) {
        void *dest = hmalloc(size);
	    memcpy(dest, ptr, old_alloc->size);    
        hfree(ptr);
        return dest;
    }
    return ptr;
}

void *hmalloc(size_t size) {
    pthread_mutex_lock(&mutex);
    stats.chunks_allocated += 1;
    size += sizeof(alloc_block_header);

    if (size < PAGE_SIZE) {
        free_block_header *free_block = get_free_block(size);
        if (free_block) {
            alloc_block_header *header = (alloc_block_header *) free_block;
            size_t free_size = free_block->size;
            header->size = free_size;
            pthread_mutex_unlock(&mutex);
            return (void *) (header + 1);
        } else {
            // Allocate a new page
            void *new_page = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            stats.pages_mapped += 1;
            size_t rest = PAGE_SIZE - size;
            if (rest >= sizeof(free_block_header)) {
                free_block_header *new_free_block = (free_block_header *) (((char *) new_page) + size);
                new_free_block->size = rest;
                insert_free_block(new_free_block);
            } else {
                size = PAGE_SIZE; 
            }
            alloc_block_header *header = (alloc_block_header *) new_page;
            header->size = size;
            pthread_mutex_unlock(&mutex);
            return (void *) (header + 1);
        }
    } else {
        // Always map a new set of pages for sizes >= PAGE_SIZE
        size_t num_pages = ceil_div(size, PAGE_SIZE);

        void *new_pages = mmap(NULL, num_pages * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (new_pages == MAP_FAILED) {
            return NULL;
        }
        stats.pages_mapped += num_pages;

        alloc_block_header *header = (alloc_block_header *) new_pages;
        header->size = num_pages * PAGE_SIZE;
        pthread_mutex_unlock(&mutex);
        return (void *) (header + 1);
    }
}

void hfree(void *item) {
	pthread_mutex_lock(&mutex);
 	stats.chunks_freed += 1;
    alloc_block_header *alloc_header = ((alloc_block_header *) item) - 1;
    size_t size = alloc_header->size;
    if (size < PAGE_SIZE) {
        free_block_header *free_header = (free_block_header *) alloc_header;
        free_header->size = size;
        insert_free_block(free_header);
    } else {
        munmap((void *) alloc_header, size);
        stats.pages_unmapped += size / PAGE_SIZE;
    }
    pthread_mutex_unlock(&mutex);
}
