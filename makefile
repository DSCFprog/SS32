CC = gcc
CFLAGS = -m32 -w -g -O3 -Wno-format
LD = -m32 -lGL -lGLU -lglut  -lpthread

build:
	$(CC) $(CFLAGS) $(LD) main.c

run: build
	./a.out

debug: build
	ddd a.out

32bit:
	source /etc/profile.d/32dev.sh

