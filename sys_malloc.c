

#include <stdlib.h>
#include <unistd.h>

#include "xmalloc.h"


void*
xmalloc(size_t bytes)
{
    printf("Malloc(%d bytes)\n", bytes);
    return malloc(bytes);
}

void
xfree(void* ptr)
{
    free(ptr);
}

void*
xrealloc(void* prev, size_t bytes)
{
    return realloc(prev, bytes);
}

