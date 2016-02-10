CC = gcc
CFLAGS = -Wall -c -O2 -g -I../
BINS = prodcon1 prodcon2 fsops pi
LIBMILL = libmill.a

all: clean $(LIBMILL) $(BINS)

prodcon1: prodcon1.o
	$(CC) -o $@ $^ $(LIBMILL) -lpthread

prodcon2: prodcon2.o
	$(CC) -o $@ $^ $(LIBMILL) -lpthread

fsops: fsops.o
	$(CC) -o $@ $^ $(LIBMILL) -pthread

pi: pi.o
	$(CC) -o $@ $^ $(LIBMILL) -pthread -lm

libmill.a:
	@files=`for p in ../*.o; do echo $$p; done | awk -vORS=' ' '1'`; \
	ar rcs libmill.a $$files ../dns/libmill_la-dns.o

.c.o:
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f *.o
	rm -f $(BINS)
	rm -f $(LIBMILL)