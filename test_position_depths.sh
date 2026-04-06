#!/usr/bin/env bash
set -euo pipefail

# Nxe5, Qf4 good moves, everything else blunder
FEN='3r2rk/pp3p1p/2p1pq2/2P1P2R/2n5/3P4/P3Q1B1/1K1R4 b - - 0 34'

for d in 1 2 3 4 5 6; do
    echo "=== depth $d ==="
    printf "uci\nisready\nposition fen %s\ngo depth %d\nquit\n" "$FEN" "$d" |
        ./chess2 | grep -E '^(info depth|bestmove|readyok|uciok)'
    echo
 done
