# Cepimetheus-Hydra

George's C# engine Epimetheus remade in C. He had numerous issues with the Epimetheus codebase, so this is the new continuation, the C# version can be mostly considered dead.

## Build

Requires GCC (or compatible C99 compiler) and Make. On Windows, install [MSYS2](https://www.msys2.org/) then run:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make
```

Then build:

```sh
mingw32-make
```

For a Windows `.exe` (cross-compile from Linux/MSYS2):

```sh
make windows
```

## Run

Start the engine binary and speak UCI over stdin/stdout:

```sh
./Cepimetheus-Hydra
```

## UCI Options

### `Hash` (default: 64, range: 1–65536)

Size of the transposition table in MB. Larger values reduce cache thrashing when using multiple threads. A good rule of thumb is ~64MB per thread.

```
setoption name Hash value 256
```

### `Threads` (default: 1, range: 1–256)

Sets the number of search threads. Uses Lazy SMP — worker threads run persistently between moves, sharing a transposition table with staggered search depths to pre-fill the TT for the main thread.

```
setoption name Threads value 4
```

Set this before sending `go`. A value of `1` is identical to single-threaded behaviour. Be mindful of your CPU's logical thread count — running two concurrent games with 4 threads each uses 8 threads total.

### `overhead` (default: 100, range: 0–10000)

Time in milliseconds subtracted from the clock budget to account for communication latency. Increase this if the engine is flagging on slow connections.

```
setoption name overhead value 50
```

## Example UCI session

```
uci
setoption name Threads value 4
setoption name Hash value 256
isready
position startpos
go movetime 5000
```

## Testing with cutechess-cli

To run a match between 1T, 2T, and 4T versions:

```sh
cutechess-cli \
  -engine name=Hydra-1T cmd=./Cepimetheus-Hydra option.Threads=1 option.Hash=64 \
  -engine name=Hydra-2T cmd=./Cepimetheus-Hydra option.Threads=2 option.Hash=128 \
  -engine name=Hydra-4T cmd=./Cepimetheus-Hydra option.Threads=4 option.Hash=256 \
  -each proto=uci tc=10+0.1 \
  -games 2 -rounds 10 -repeat -concurrency 2 \
  -pgnout results.pgn
```

On a 6-core/12-thread CPU, keep `Threads × concurrency ≤ 10` to avoid oversubscription.
