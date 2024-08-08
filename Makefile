CC = gcc
CFLAGS = -Wall -W -pedantic -std=c99

all: mini-redis-server

mini-redis-server: main.c
	$(CC) $(CFLAGS) -o mini-redis-server main.c

clean:
	rm -f mini-redis-server
