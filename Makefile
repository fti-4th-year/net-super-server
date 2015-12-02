.phony: all

all: server

server: main.o
	gcc $^ -o $@

main.o: main.c
	gcc -c $< -o $@
