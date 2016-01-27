CC = gcc
CFLAGS = -Wall -c -O2 -g -I../
BINS = prodcon1 prodcon2
LIBS = libmill.a

all: $(LIBS) $(BINS)

prodcon1: prodcon1.o
	$(CC) -o $@ $^ $(LIBS) -lpthread

prodcon2: prodcon2.o
	$(CC) -o $@ $^ $(LIBS) -lpthread

libmill.a:
	@files=`for p in ../*.o; do echo $$p; done | awk -vORS=' ' '1'`; \
	ar rcs libmill.a $$files ../dns/libmill_la-dns.o

.c.o:
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f *.o
	rm -f $(BINS)
