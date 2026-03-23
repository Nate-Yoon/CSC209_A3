CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g -DPORT=$(PORT)
PORT ?= 4242

COMMON_OBJS = protocol.o game.o
SERVER_OBJS = server.o $(COMMON_OBJS)
CLIENT_OBJS = client.o protocol.o
TARGETS = server client

.PHONY: all clean

all: $(TARGETS)

server: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_OBJS)

client: $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $(CLIENT_OBJS)

server.o: server.c server.h protocol.h game.h
client.o: client.c client.h protocol.h
protocol.o: protocol.c protocol.h
game.o: game.c game.h protocol.h

clean:
	rm -f $(TARGETS) *.o
