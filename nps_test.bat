@echo off
setlocal enabledelayedexpansion

set ENGINE=Cepimetheus-Hydra.exe
set FEN=3r2rk/pp3p1p/2p1pq2/2P1P2R/2n5/3P4/P3Q1B1/1K1R4 b - - 0 34
set THREADS=1
set HASH=64
set MOVETIME=5000

echo ============================================================
echo  NPS + best-move test
echo  Engine  : %ENGINE%
echo  Threads : %THREADS%
echo  Hash    : %HASH% MB
echo  Movetime: %MOVETIME% ms
echo  FEN     : %FEN%
echo ============================================================
echo.

(
    echo uci
    echo setoption name Threads value %THREADS%
    echo setoption name Hash value %HASH%
    echo isready
    echo position fen %FEN%
    echo go movetime %MOVETIME%
    echo quit
) | "%ENGINE%" | findstr /r /c:"^info depth" /c:"^bestmove"

echo.
echo ============================================================
echo  Depth sweep  ^(depth 1-8, best move + NPS per depth^)
echo ============================================================
echo.

for %%d in (1 2 3 4 5 6 7 8) do (
    echo === depth %%d ===
    (
        echo uci
        echo setoption name Threads value %THREADS%
        echo setoption name Hash value %HASH%
        echo isready
        echo position fen %FEN%
        echo go depth %%d
        echo quit
    ) | "%ENGINE%" | findstr /r /c:"^info depth" /c:"^bestmove"
    echo.
)
