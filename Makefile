BINS := list-test ivec-test

HDRS := $(wildcard *.h)
SRCS := $(wildcard *.c)
OBJS := $(SRCS:.c=.o)

CFLAGS := -g -std=gnu11
LDLIBS := -lpthread

all: $(BINS)

list-test: list_main.o xmalloc.o
	gcc $(CFLAGS) -o $@ $^ $(LDLIBS)

ivec-test: ivec_main.o xmalloc.o
	gcc $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o : %.c $(HDRS) Makefile

clean:
	rm -f *.o $(BINS)

.PHONY: clean test
