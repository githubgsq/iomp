LIB=libiomp.a

CC=cc
CFLAGS=-std=c99 -g -Wall -pipe -fPIC -fvisibility=hidden
CXX=c++
CXXFLAGS=-std=c++11 -g -Wall -pipe -fPIC -fvisibility=hidden
AR=ar
ARFLAGS=rc
LD=c++
LDFLAGS=-lpthread

all: $(LIB) test

.PHONY: clean
clean:
	rm -f $(LIB) iomp.o iomp_kqueue.o iomp_epoll.o test test.o

rebuild: clean all

$(LIB): iomp.o iomp_kqueue.o iomp_epoll.o
	$(AR) $(ARFLAGS) $@ iomp.o iomp_kqueue.o iomp_epoll.o

test: test.o $(LIB)
	$(LD) -o $@ test.o -L. -liomp $(LDFLAGS)

iomp.o: iomp.c
	$(CC) -c $(CFLAGS) -o $@ $<

iomp_kqueue.o: iomp_kqueue.c
	$(CC) -c $(CFLAGS) -o $@ $<

iomp_epoll.o: iomp_epoll.c
	$(CC) -c $(CFLAGS) -o $@ $<

test.o: test.cc
	$(CXX) -c $(CXXFLAGS) -o $@ $<

