# allocator

A memory allocator in C written as course work with Elleen Pan (elleenpan.com)
for Northeastern's CS 3560 - Computer Systems in Fall 2017.

## Reflections

These are reflections on the design and performance of the allocator implementation:

The general idea of our allocator is a simplified tcmalloc with a few trade-offs
made for the use case at hand. At a high level, there is a central page stack
which mmap's a lot of pages from the system at once and then hands them out to
threads, each of whichs maintains a binned cache of free cells segregated by size. Allocations
larger than half a page are automatically mmap'd directly, bypassing both the thread cache
and central page stack. Small allocations made per thread can just be pulled from the thread
cache without locking other threads. The only locking occurs when manipulating the system page stack.

The trade-offs that our design makes fall largely in line with the trade-offs that tcmalloc also makes.
The biggest of these is that our allocator both grabs a lot of memory from the system at startup and
doesn't ever give it back until termination. This is true of tcmalloc as well. Our allocator doesn't
really make sense for applications that don't use a lot of memory, because it will grab a lot more than
it will ever use and keep it for the life of the process.

The second trade-off is that cross-thread frees aren't particularly elegant. They work but in freeing
memory, the thread the calls free assumes ownership of the newly freed block and adds it to its own cache.
This would probably have somewhat strange performance implications under certain usage profiles.

Finally, our allocator has a naive handling of large allocations, which it just delegates to mmap. This
works for the intended use-cases but might have negative implications for certain usage profiles.
