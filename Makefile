
CC ?= cc
CLFAGS ?= -Wall -Wextra -g -I/usr/include -I/usr/local/include -L/usr/lib -L/usr/local/lib -O2

all: gwdown

gwdown: gwdown.c
	$(CC) $(CLFAGS) -o gwdown gwdown.c -lcurl -lpthread

clean:
	rm -f gwdown

.PHONY: clean
