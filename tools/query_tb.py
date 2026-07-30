'''
This file is part of draughts-tb.

draughts-tb is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

draughts-tb is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with draughts-tb. If not, see <https://www.gnu.org/licenses/>.
'''
#!/usr/bin/env python3
import argparse
import hashlib
import pathlib
import struct
import sys

HEADER_FORMAT = "<8sIIIIQQB23s"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)

LOSS = 0
DRAW = 1
WIN = 2
INVALID = 3

VALUE_NAMES = {
    LOSS: "loss",
    DRAW: "draw",
    WIN: "win",
    INVALID: "invalid",
}

WHITE_MAN = 0
WHITE_KING = 1
BLACK_MAN = 2
BLACK_KING = 3


def choose(n: int, k: int) -> int:
    if k < 0 or k > n:
        return 0
    k = min(k, n - k)
    value = 1
    for i in range(1, k + 1):
        value = value * (n - k + i) // i
    return value


def four_power(n: int) -> int:
    return 4 ** n


def parse_square_list(text: str):
    if not text:
        return []
    return [int(item) - 1 for item in text.split(",") if item]


def build_pieces(args):
    pieces = []

    for square in parse_square_list(args.white_men):
        pieces.append((square, WHITE_MAN))
    for square in parse_square_list(args.white_kings):
        pieces.append((square, WHITE_KING))
    for square in parse_square_list(args.black_men):
        pieces.append((square, BLACK_MAN))
    for square in parse_square_list(args.black_kings):
        pieces.append((square, BLACK_KING))

    pieces.sort()

    if len({square for square, _ in pieces}) != len(pieces):
        raise ValueError("two pieces occupy the same square")

    return pieces


def rank_position(pieces, side: str) -> int:
    occupancy_rank = sum(
        choose(square, i + 1)
        for i, (square, _) in enumerate(pieces)
    )

    type_code = 0
    multiplier = 1

    for _, piece_type in pieces:
        type_code += multiplier * piece_type
        multiplier *= 4

    side_code = 0 if side == "white" else 1

    return (
        (occupancy_rank * four_power(len(pieces)) + type_code) * 2
        + side_code
    )


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        while True:
            data = file.read(1024 * 1024)
            if not data:
                break
            digest.update(data)
    return digest.hexdigest()


def verify_sidecar(path: pathlib.Path):
    sidecar = pathlib.Path(str(path) + ".sha256")
    if not sidecar.exists():
        raise RuntimeError(f"missing SHA sidecar: {sidecar}")

    expected = sidecar.read_text(encoding="ascii").split()[0].lower()
    actual = sha256_file(path)

    if expected != actual:
        raise RuntimeError(
            f"SHA-256 mismatch\nexpected: {expected}\nactual:   {actual}"
        )

    print(f"SHA-256 OK: {actual}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(
        description="Query a Brazilian/international draughts WLD tablebase."
    )

    parser.add_argument("tablebase", type=pathlib.Path)
    parser.add_argument(
        "--side",
        choices=("white", "black"),
        required=True,
        help="side to move",
    )
    parser.add_argument(
        "--white-men",
        default="",
        help="comma-separated, one-based playable-square numbers",
    )
    parser.add_argument(
        "--white-kings",
        default="",
        help="comma-separated, one-based playable-square numbers",
    )
    parser.add_argument(
        "--black-men",
        default="",
        help="comma-separated, one-based playable-square numbers",
    )
    parser.add_argument(
        "--black-kings",
        default="",
        help="comma-separated, one-based playable-square numbers",
    )
    parser.add_argument(
        "--skip-sha",
        action="store_true",
        help="do not check the .sha256 sidecar",
    )

    args = parser.parse_args()

    if not args.skip_sha:
        verify_sidecar(args.tablebase)

    pieces = build_pieces(args)

    with args.tablebase.open("rb") as file:
        header_data = file.read(HEADER_SIZE)
        if len(header_data) != HEADER_SIZE:
            raise RuntimeError("truncated tablebase header")

        (
            magic,
            version,
            board_size,
            piece_count,
            playable_squares,
            entries,
            data_offset,
            variant,
            _reserved,
        ) = struct.unpack(HEADER_FORMAT, header_data)

        if not magic.startswith(b"WLDDB1"):
            raise RuntimeError("invalid tablebase magic")
        if version != 1:
            raise RuntimeError(f"unsupported version: {version}")
        if len(pieces) != piece_count:
            raise RuntimeError(
                f"table requires exactly {piece_count} pieces; "
                f"position contains {len(pieces)}"
            )

        for square, _ in pieces:
            if square < 0 or square >= playable_squares:
                raise RuntimeError(
                    f"square {square + 1} is outside this tablebase"
                )

        index = rank_position(pieces, args.side)

        if index >= entries:
            raise RuntimeError("calculated index is outside the table")

        byte_offset = data_offset + index // 4
        shift = (index % 4) * 2

        file.seek(byte_offset)
        raw = file.read(1)
        if len(raw) != 1:
            raise RuntimeError("truncated tablebase data")

        value = (raw[0] >> shift) & 3

    variant_name = "brazilian" if variant == 0 else "international"

    print(f"variant: {variant_name}")
    print(f"board: {board_size}x{board_size}")
    print(f"index: {index}")
    print(f"side-to-move result: {VALUE_NAMES[value]}")


if __name__ == "__main__":
    try:
        main()
    except Exception as exception:
        print(f"error: {exception}", file=sys.stderr)
        sys.exit(1)
