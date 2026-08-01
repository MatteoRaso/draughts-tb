# DWTB

DWTB generates history-free WLD tablebases for:

- Brazilian draughts: international movement and capture rules on 8x8.
- International draughts: the same engine on 10x10.

## Rule details

The move generator implements:

1. Captures are mandatory.
2. Only sequences capturing the maximum number of pieces are legal.
3. Men capture in all four diagonal directions.
4. Kings are flying kings.
5. A captured piece remains on the board as a blocker until the sequence ends.
6. A captured piece cannot be jumped more than once.
7. A landing square may be revisited.
8. Closed capture loops are legal.
9. A man remains a man throughout a multiple capture.
10. Promotion occurs only if the man finishes the complete turn on its
    promotion row.

## WLD model

The database stores history-free game-graph WLD values.

It does not encode repetition counters or a move-count draw clock. If an
application uses history-dependent draw adjudication, that state must be
handled above the tablebase layer.

## Build

Dependencies:

- C++20 compiler
- CMake 3.20 or newer
- OpenSSL
- Zstandard

Linux example:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Generate Brazilian tablebases

```sh
./build/dwtb-generate \
    --board brazilian \
    --pieces 6 \
    --threads 32 \
    --out /tablebases/brazilian-6
```

## Generate international tablebases

```sh
./build/dwtb-generate \
    --board international \
    --pieces 5 \
    --threads 32 \
    --out /tablebases/international-5
```

`--pieces N` means generate all dependencies through N pieces.

## Slice order

Each file has an exact material vector:

```text
b10-wm3-wk1-bm2-bk1.dwtb
```

This means:

- 3 white men
- 1 white king
- 2 black men
- 1 black king

Generation proceeds by:

1. Increasing total piece count.
2. Decreasing total king count within a piece count.

That ordering is valid because:

- Captures lead to a lower-piece tablebase.
- Promotions lead to a same-piece-count slice with one more king.
- Other quiet moves remain in the current exact material slice.

A material vector and its 180-degree/color-swapped equivalent share one
physical file. Self-symmetric material vectors are not reduced this way.

## Temporary storage

The generator uses a memory-mapped 32-bit cell per logical state while
solving a slice. Frontiers are stored as disk files, not retained in RAM.

Put temporary data on fast NVMe storage:

```sh
./build/dwtb-generate \
    --board 10 \
    --pieces 6 \
    --threads 64 \
    --out /slow-storage/final \
    --temp /fast-nvme/dwtb-temp
```

The final format is much smaller than the generation workspace.

## Compression

Final values use two bits:

```text
00 loss
01 draw
10 win
11 reserved/unknown
```

Each block is independently encoded as one of:

- Constant value
- Raw two-bit data
- Zstandard-compressed two-bit data

The smallest representation is selected.

By default, capture positions are not stored directly. The query driver
recognizes them, generates all legal maximum-capture continuations, and
probes their lower-piece children. During compression, capture entries are
replaced with whichever of loss, draw, or win compresses the block best.

This behavior can be disabled with:

```sh
--store-captures
```

Storing captures may improve probe latency but usually increases file size.

Larger `--block-log2` values generally improve compression and reduce index
overhead, at the cost of more decompression work on a cache miss.

## Integrity

Every output file has a SHA-256 sidecar:

```text
b8-wm2-wk0-bm2-bk0.dwtb
b8-wm2-wk0-bm2-bk0.dwtb.sha256
```

A `manifest.json` and `manifest.json.sha256` are also generated.

On Linux:

```sh
cd /tablebases/brazilian-6
sha256sum -c *.sha256
```

The driver does not automatically hash files at startup, because hashing
multi-terabyte databases would make startup unnecessarily slow.

## C++ queries

```cpp
#include "dwtb/dwtb.hpp"

dwtb::Tablebase tb("/tablebases/brazilian-6", 8, 1024);

dwtb::Position p;
p.white = /* contiguous playable-square mask */;
p.black = /* contiguous playable-square mask */;
p.kings = /* subset of white | black */;

dwtb::Value value = tb.probe(p, dwtb::Color::White);
```

Playable squares are numbered in row-major order:

- 8x8: 1 through 32
- 10x10: 1 through 50

Bit zero corresponds to square 1.

## Command-line query

```sh
./build/dwtb-query \
    --db /tablebases/brazilian-6 \
    --board 8 \
    --white 0x10000 \
    --black 0x200 \
    --kings 0x10000 \
    --turn white
```

## Python query

```sh
python3 python/query_tb.py \
    --lib build/libdwtb.so \
    --db /tablebases/brazilian-6 \
    --board 8 \
    --turn white \
    --white-men 21 \
    --black-men 14
```

Multiple squares are comma-separated:

```sh
--white-men 21,22 --white-kings 7
```

## Normal tests

```sh
ctest --test-dir build --output-on-failure
```

The rule tests cover:

- Mandatory captures
- Maximum capture length
- No piece captured twice
- Flying kings
- Capture loops and repeated landing squares
- End-of-turn-only promotion
- Backward captures by men
- Rank/unrank consistency

The smoke test generates complete two-piece Brazilian tablebases and checks
the WLD Bellman condition for every position.

## Exhaustive Kingsrow five-piece test

Clone or download the Kingsrow driver:

```sh
git clone https://github.com/eygilbert/egdb_intl.git
```

Configure:

```sh
cmake -S . -B build-kingsrow \
    -DCMAKE_BUILD_TYPE=Release \
    -DDWTB_WITH_KINGSROW=ON \
    -DDWTB_KINGSROW_SOURCE=/src/egdb_intl \
    -DDWTB_KINGSROW_DB=/db/kingsrow-wld-v2
```

The Kingsrow directory must contain:

```text
db2.cpr1  db2.idx1
db3.cpr1  db3.idx1
db4.cpr1  db4.idx1
db5.cpr1  db5.idx1
```

Build and run:

```sh
cmake --build build-kingsrow -j

./build-kingsrow/kingsrow_compare5 \
    --kingsrow /db/kingsrow-wld-v2 \
    --out /tablebases/international-5 \
    --threads 32
```

This test:

1. Generates all tablebases through five pieces.
2. Enumerates every valid exact five-piece material slice.
3. Checks both sides to move.
4. Resolves capture positions recursively.
5. Probes non-capture positions through the Kingsrow driver.
6. Stops at the first discrepancy and prints the bitboards and material rank.

This is an exhaustive offline validation test and is not intended for a
normal short CI run.

## Recovery and resumption

Completed slices are skipped unless `--force` is supplied. Therefore a
large run can be resumed by invoking the same command again.

A slice is written only after retrograde analysis and compression finish.
Temporary `.state` and frontier files are deleted afterward unless
`--keep-temp` is used.

Before trusting a resumed run, verify all SHA-256 sidecars.
