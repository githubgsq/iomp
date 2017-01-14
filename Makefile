LIB=libiomp.a
SRC=$(wildcard *.c)
OBJ=$(patsubst %.c,%.o,$(SRC))

CC=cc
CFLAGS=-std=c11 -g -Wall -pipe -fPIC -fvisibility=hidden
CXX=c++
CXXFLAGS=-std=c++11 -g -Wall -pipe -fPIC -fvisibility=hidden
AR=ar
ARFLAGS=rc
LDFLAGS=-lpthread

all: $(LIB) test

.PHONY: clean
clean:
	rm -f $(LIB) $(OBJ) test

rebuild: clean all

$(LIB): $(OBJ)
	$(AR) $(ARFLAGS) $@ $^

test: test.cc $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) -c $(CFLAGS) -o $@ $<
