# Cepimetheus-Hydra

George's C# engine Epimetheus remade in C. He had numerous issues with the Epimetheus codebase, so this is the new continuation, the C# version can be mostly considered dead.

I have used Claude to (hopefully) very lazily add multithreading.

## Build

Use the provided `Makefile` with a POSIX-style C toolchain:

```sh
make
```

## Run

Start the engine binary and speak UCI over stdin/stdout:

```sh
./Cepimetheus
```
