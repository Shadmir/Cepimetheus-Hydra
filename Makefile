CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -Iinclude
WIN_CC ?= x86_64-w64-mingw32-gcc
WIN32_CC ?= i686-w64-mingw32-gcc

TARGET := chess2
SRC := \
	src/main.c \
	src/uci.c \
	src/think.c \
	src/movegen.c \
	src/move.c \
	src/board.c \
	src/bitboard.c

OBJ := $(SRC:.c=.o)

.PHONY: all clean run windows windows32

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $^ -o $@

run: $(TARGET)
	./$(TARGET)

windows:
	$(MAKE) clean
	$(MAKE) CC=$(WIN_CC) TARGET=chess2.exe

windows32:
	$(MAKE) clean
	$(MAKE) CC=$(WIN32_CC) TARGET=chess2.exe

clean:
	rm -f $(OBJ) $(TARGET) chess2.exe
