LIB=libiomp.a

CC=cc
CFLAGS=-std=c99 -g -Wall -pipe -fPIC -fvisibility=hidden
CXX=c++
CXXFLAGS=-std=c++14 -g -Wall -pipe -fPIC -fvisibility=hidden
AR=ar
ARFLAGS=rc
LD=c++
LDFLAGS=-lpthread

all: $(LIB) test

.PHONY: clean
clean:
	rm -f $(LIB) iomp.o test test.o

rebuild: clean all

$(LIB): iomp.o
	$(AR) $(ARFLAGS) $@ iomp.o

test: test.o $(LIB)
	$(LD) -o $@ test.o -L. -liomp $(LDFLAGS)

iomp.o: iomp.c
	$(CC) -c $(CFLAGS) -o $@ $<

test.o: test.cc
	$(CXX) -c $(CXXFLAGS) -o $@ $<

