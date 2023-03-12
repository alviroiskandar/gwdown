
CC ?= cc
CLFAGS ?= -Wall -Wextra -O2 -g -I/usr/include -I/usr/local/include -L/usr/lib -L/usr/local/lib -O2 -D_GNU_SOURCE

all: gwdown

gwdown: gwdown.c
	$(CC) $(CLFAGS) -o gwdown gwdown.c -lcurl

clean:
	rm -f gwdown

.PHONY: clean
