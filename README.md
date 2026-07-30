# Draughts WLD tablebase generator

This project generates exact-piece-count WLD tablebases for:

- Brazilian draughts: 8x8 board, 32 playable squares.
- International draughts: 10x10 board, 50 playable squares.

## Build

Dependencies:

- CMake 3.20 or newer
- C++20 compiler
- OpenSSL development package
- POSIX threads or Windows thread support through CMake

On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake libssl-dev
```

Build and test:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Generate Brazilian tablebases

The following generates one-, two-, three-, and four-piece files:

```sh
mkdir -p tables

./build/wldgen generate \
    --variant brazilian \
    --pieces 4 \
    --threads 16 \
    --output tables/brazilian
```

Files produced:

```text
tables/brazilian.1.wld
tables/brazilian.1.wld.sha256
tables/brazilian.2.wld
tables/brazilian.2.wld.sha256
...
```

## Generate international tablebases

```sh
./build/wldgen generate \
    --variant international \
    --pieces 3 \
    --threads 16 \
    --output tables/international
```

## Query with Python

Playable squares are numbered from 1 in row-major order over the dark
squares. This gives 1-32 for Brazilian draughts and 1-50 for
international draughts.

Example:

```sh
python3 tools/query_tb.py tables/brazilian.3.wld \
    --side white \
    --white-kings 21 \
    --black-men 14,18
```

Piece options:

- `--white-men`
- `--white-kings`
- `--black-men`
- `--black-kings`

Each accepts a comma-separated list of one-based playable-square
numbers.

The Python driver checks the SHA-256 sidecar by default. Use
`--skip-sha` only when integrity checking is not wanted.
