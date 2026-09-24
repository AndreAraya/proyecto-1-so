CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic
LDLIBS = -lcrypto

.PHONY: all clean
all: bin/serial bin/fork bin/pthread bin/gui

bin/serial: src/serial/serial.c
	mkdir -p bin
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

bin/fork: src/fork/fork.c src/serial/serial.c
	mkdir -p bin
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

bin/pthread: src/pthread/pthread.c src/serial/serial.c
	mkdir -p bin
	$(CC) $(CFLAGS) -pthread $< -o $@ $(LDLIBS)

bin/gui: src/gui/gui.c
	mkdir -p bin
	$(CC) $(CFLAGS) $(shell pkg-config --cflags gtk4) $< -o $@ $(shell pkg-config --libs gtk4)

clean:
	rm -f bin/serial bin/fork bin/pthread bin/gui
