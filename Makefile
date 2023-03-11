
CC ?= cc
CLFAGS ?= -Wall -Wextra -O2 -g

all: gwdown

gwdown: gwdown.c
	$(CC) $(CLFAGS) -o gwdown gwdown.c -lcurl

clean:
	rm -f gwdown

.PHONY: clean
