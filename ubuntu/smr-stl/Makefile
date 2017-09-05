CFLAGS=-g -O0 -Wall

all: stl format mkfakesmr stl-plugin.so

stl: stl_map.o stl_base.o rb.o stl_test.o stl_fakesmr.o
	gcc -g $^ -o $@

format: format.o stl_fakesmr.o
	gcc -g $^ -o $@

mkfakesmr: mkfakesmr.o stl_fakesmr.o
	gcc -g $^ -o $@

clean:
	rm -f *.o stl stl2

SHARED_OBJS = stl-plugin.shared.o stl_map.shared.o stl_base.shared.o \
	rb.shared.o stl_fakesmr.shared.o
stl-plugin.so: $(SHARED_OBJS)
	gcc -shared -fPIC -DPIC $^ -o $@

%.shared.o : %.c
	gcc -fPIC -DPIC -g -O0 -c $^ -o $@ -DRBTEST

stl2: stl2.o rb.o
	gcc -g -O0 stl2.o rb.o -o $@

rb.o: rb.c
	gcc -g -c -O0 rb.c -DRBTEST


