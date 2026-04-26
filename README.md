# Cepimetheus-Hydra

George's C# engine Epimetheus remade in C. He had numerous issues with the Epimetheus codebase, so this is the new continuation, the C# version can be mostly considered dead.

## Build

Requires GCC (or compatible C99 compiler) and Make. On Windows, install [MSYS2](https://www.msys2.org/) then run:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make
```

Then build:

```sh
make
```

For a Windows `.exe` (cross-compile from Linux/MSYS2):

```sh
make windows
```

## Run

Start the engine binary and speak UCI over stdin/stdout:

```sh
./Cepimetheus
```

## UCI Options

### `Threads` (default: 1, range: 1–256)

Sets the number of search threads. Uses Lazy SMP — all threads search the same position independently and share a transposition table, improving strength on multi-core hardware.

```
setoption name Threads value 4
```

Set this before sending `go`. A value of `1` is identical to the original single-threaded behaviour.

### `overhead` (default: 100, range: 0–10000)

Time in milliseconds subtracted from the clock budget to account for communication latency. Increase this if the engine is flagging on slow connections.

```
setoption name overhead value 50
```

## Example UCI session

```
uci
setoption name Threads value 4
isready
position startpos
go movetime 5000
```

## Testing with cutechess-cli

To run a match between single-threaded and 4-thread versions:

```sh
cutechess-cli \
  -engine name=Cep1T cmd=./Cepimetheus option.Threads=1 \
  -engine name=Cep4T cmd=./Cepimetheus option.Threads=4 \
  -each proto=uci tc=10+0.1 \
  -games 400 -concurrency 2 -repeat \
  -pgnout results.pgn
```
