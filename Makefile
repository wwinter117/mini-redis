all: mini-redis-server

mini-redis-server: main.c
	$(CC) -o mini-redis-server main.c -Wall -W -pedantic -std=c99

clean:
	rm mini-redis-server
