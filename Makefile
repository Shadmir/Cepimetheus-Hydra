CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -Iinclude

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

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $^ -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)
