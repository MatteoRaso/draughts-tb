'''
draughts-tb: A tablebase generator for Brazilian and international draughts.
    Copyright (C) 2026  Matteo Raso

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
'''
#!/usr/bin/env python3

import argparse
import ctypes
import json
import os
import platform
import sys


VALUE_NAMES = {
    0: "loss",
    1: "draw",
    2: "win",
    3: "unknown",
}


def default_library_name():
    system = platform.system()
    if system == "Windows":
        return "dwtb.dll"
    if system == "Darwin":
        return "libdwtb.dylib"
    return "libdwtb.so"


def parse_squares(text, maximum):
    """
    Parses a comma-separated list of 1-based playable square numbers.
    Example: "1,7,23".
    """
    result = 0

    if not text:
        return result

    for token in text.split(","):
        square = int(token.strip())
        if not 1 <= square <= maximum:
            raise ValueError(
                f"square {square} is outside 1..{maximum}"
            )
        result |= 1 << (square - 1)

    return result


def main():
    parser = argparse.ArgumentParser(
        description="Query a DWTB WLD tablebase"
    )

    parser.add_argument("--lib", default=default_library_name())
    parser.add_argument("--db", required=True)
    parser.add_argument(
        "--board",
        type=int,
        choices=(8, 10),
        default=8,
    )
    parser.add_argument(
        "--turn",
        choices=("white", "black"),
        required=True,
    )

    parser.add_argument("--white-men", default="")
    parser.add_argument("--white-kings", default="")
    parser.add_argument("--black-men", default="")
    parser.add_argument("--black-kings", default="")

    parser.add_argument(
        "--cache-mib",
        type=int,
        default=512,
    )

    args = parser.parse_args()

    maximum = args.board * args.board // 2

    white_men = parse_squares(args.white_men, maximum)
    white_kings = parse_squares(args.white_kings, maximum)
    black_men = parse_squares(args.black_men, maximum)
    black_kings = parse_squares(args.black_kings, maximum)

    white = white_men | white_kings
    black = black_men | black_kings
    kings = white_kings | black_kings

    if white & black:
        raise SystemExit("White and black overlap")

    library = ctypes.CDLL(os.path.abspath(args.lib))

    library.dwtb_open.argtypes = [
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_uint,
    ]
    library.dwtb_open.restype = ctypes.c_void_p

    library.dwtb_probe.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint64,
        ctypes.c_uint64,
        ctypes.c_uint64,
        ctypes.c_int,
    ]
    library.dwtb_probe.restype = ctypes.c_int

    library.dwtb_last_error.argtypes = [ctypes.c_void_p]
    library.dwtb_last_error.restype = ctypes.c_char_p

    library.dwtb_close.argtypes = [ctypes.c_void_p]
    library.dwtb_close.restype = None

    handle = library.dwtb_open(
        os.fsencode(args.db),
        args.board,
        args.cache_mib,
    )

    if not handle:
        raise SystemExit("Could not open tablebase")

    try:
        result = library.dwtb_probe(
            handle,
            white,
            black,
            kings,
            0 if args.turn == "white" else 1,
        )

        if result < 0:
            error = library.dwtb_last_error(handle)
            raise SystemExit(error.decode("utf-8", "replace"))

        print(json.dumps({
            "value": VALUE_NAMES.get(result, "invalid"),
            "code": result,
            "white": f"0x{white:x}",
            "black": f"0x{black:x}",
            "kings": f"0x{kings:x}",
            "turn": args.turn,
        }, indent=2))
    finally:
        library.dwtb_close(handle)


if __name__ == "__main__":
    main()
