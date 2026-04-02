# Chess2

A small C framework for a UCI-compatible chess engine.

## Build

Use the provided `Makefile` with a POSIX-style C toolchain:

```sh
make
```

## Run

Start the engine binary and speak UCI over stdin/stdout:

```sh
./chess2
```

## Current shape

- Bitboard-based board state
- Legal move generation
- Board mutation with legality checking
- UCI loop
- Random legal move thinker in `src/think.c`

To customize the engine, replace the logic in `src/think.c` while keeping the UCI and board layers intact.
