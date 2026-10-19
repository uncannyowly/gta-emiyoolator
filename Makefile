# gtashell: 32-bit x86 Linux. Build with scripts/build-shell.sh (Debian i386
# container, glibc 2.36) for the Mini; the result also runs on a PC directly.
CC ?= gcc
CFLAGS = -m32 -std=gnu99 -O2 -g -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -Wall -Wextra -Wno-unused-parameter \
	-fno-strict-aliasing -fno-omit-frame-pointer -fno-pie \
	-mincoming-stack-boundary=2
LDFLAGS = -m32 -no-pie
LIBS = -ldl -lpthread -lm

SRC = src/main.c src/pe.c src/files.c src/kernel32.c src/user32.c src/misc.c src/mss32.c \
	src/ddraw.c src/wdir.c src/video.c src/input.c
OBJ = $(SRC:src/%.c=build/shell/%.o)

all: build/gtashell

build/shell/%.o: src/%.c src/shell.h
	@mkdir -p build/shell
	$(CC) $(CFLAGS) -c $< -o $@

build/gtashell: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

clean:
	rm -rf build/shell build/gtashell

.PHONY: all clean
