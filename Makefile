CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -Iinclude
WIN_CC ?= x86_64-w64-mingw32-gcc
WIN32_CC ?= i686-w64-mingw32-gcc

TARGET := Cepimetheus
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
	$(MAKE) CC=$(WIN_CC) TARGET=Cepimetheus.exe


clean:
	rm -f $(OBJ) $(TARGET) Cepimetheus.exe
