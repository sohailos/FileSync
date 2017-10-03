PORT=57653
CFLAGS = -DPORT=$(PORT) -g -Wall -std=gnu99
DEPENDENCIES = hash.h ftree.h

all: rcopy_server rcopy_client

rcopy_server: rcopy_server.o server.o hash_functions.o
	gcc ${CFLAGS} -o $@ $^

rcopy_client: rcopy_client.o client.o hash_functions.o
	gcc ${CFLAGS} -o $@ $^

%.o: %.c {DEPENDENCIES}
	gcc ${CFLAGS} -c $<

clean:
	rm -f *.o rcopy_server rcopy_client
