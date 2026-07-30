/*
 This file is part of draughts-tb.

draughts-tb is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

draughts-tb is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with draughts-tb. If not, see <https://www.gnu.org/licenses/>.
*/
#include "wld.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace draughts;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int square(const BoardGeometry& geometry, int row, int column) {
    const int result = geometry.square_at(row, column);
    require(result >= 0, "test used a non-playable square");
    return result;
}

PlacedPiece at(
    const BoardGeometry& geometry,
    int row,
    int column,
    Piece piece
) {
    return PlacedPiece{
        static_cast<std::uint8_t>(square(geometry, row, column)),
        piece
    };
}

void test_mandatory_capture() {
    BoardGeometry g(Variant::Brazilian);

    Position p{
        Variant::Brazilian,
        Side::White,
        {
            at(g, 5, 0, Piece::WhiteMan),
            at(g, 5, 4, Piece::WhiteMan),
            at(g, 4, 1, Piece::BlackMan)
        }
    };

    const auto moves = legal_moves(p);

    require(!moves.empty(), "mandatory-capture test has no moves");
    for (const auto& move : moves) {
        require(move.is_capture(),
                "quiet move returned while a capture exists");
    }

    require(moves.size() == 1,
            "unexpected number of mandatory captures");
}

void test_longest_capture_sequence() {
    BoardGeometry g(Variant::Brazilian);

    Position p{
        Variant::Brazilian,
        Side::White,
        {
            at(g, 5, 0, Piece::WhiteMan),
            at(g, 5, 4, Piece::WhiteMan),

            at(g, 4, 1, Piece::BlackMan),
            at(g, 2, 3, Piece::BlackMan),

            at(g, 4, 5, Piece::BlackMan)
        }
    };

    const auto moves = legal_moves(p);

    require(!moves.empty(), "longest-capture test has no moves");

    for (const auto& move : moves) {
        require(move.capture_count() == 2,
                "shorter capture sequence was not filtered out");
        require(move.from ==
                    square(g, 5, 0),
                "capture from the wrong piece survived filtering");
    }
}

void test_cannot_capture_same_piece_twice() {
    BoardGeometry g(Variant::Brazilian);

    Position p{
        Variant::Brazilian,
        Side::White,
        {
            at(g, 5, 0, Piece::WhiteKing),
            at(g, 3, 2, Piece::BlackMan)
        }
    };

    const auto moves = legal_moves(p);

    require(!moves.empty(), "king recapture test has no captures");

    for (const auto& move : moves) {
        require(move.capture_count() == 1,
                "the same piece was captured more than once");
    }
}

void test_flying_king_quiet_moves() {
    BoardGeometry g(Variant::Brazilian);

    Position p{
        Variant::Brazilian,
        Side::White,
        {
            at(g, 5, 2, Piece::WhiteKing),
            at(g, 0, 1, Piece::BlackMan)
        }
    };

    const auto moves = legal_moves(p);

    bool found_long_move = false;

    for (const auto& move : moves) {
        const auto [from_row, from_column] =
            g.coordinates(move.from);
        const auto [to_row, to_column] =
            g.coordinates(move.to);

        if (std::abs(from_row - to_row) > 1 &&
            std::abs(from_column - to_column) > 1) {
            found_long_move = true;
        }
    }

    require(found_long_move,
            "flying king could not make a long quiet move");
}

void test_flying_king_capture_landings() {
    BoardGeometry g(Variant::Brazilian);

    Position p{
        Variant::Brazilian,
        Side::White,
        {
            at(g, 7, 0, Piece::WhiteKing),
            at(g, 4, 3, Piece::BlackMan)
        }
    };

    const auto moves = legal_moves(p);

    require(moves.size() >= 2,
            "flying king was not offered multiple landing squares");

    for (const auto& move : moves) {
        require(move.capture_count() == 1,
                "unexpected flying-king capture count");
    }
}

void test_man_captures_backwards() {
    BoardGeometry g(Variant::Brazilian);

    Position p{
        Variant::Brazilian,
        Side::White,
        {
            at(g, 3, 2, Piece::WhiteMan),
            at(g, 4, 3, Piece::BlackMan)
        }
    };

    const auto moves = legal_moves(p);

    require(moves.size() == 1,
            "backward man capture was not generated uniquely");
    require(moves.front().is_capture(),
            "backward man move was not a capture");
    require(moves.front().to ==
                square(g, 5, 4),
            "backward man capture has wrong landing square");
}

void test_promotion_at_end_of_move() {
    BoardGeometry g(Variant::Brazilian);

    Position p{
        Variant::Brazilian,
        Side::White,
        {
            at(g, 2, 1, Piece::WhiteMan),
            at(g, 1, 2, Piece::BlackMan)
        }
    };

    const auto moves = legal_moves(p);
    require(moves.size() == 1, "promotion test has wrong moves");

    const Position result = apply_move(p, moves.front());

    bool found_king = false;
    for (const auto& placed : result.pieces) {
        if (placed.square == square(g, 0, 3) &&
            placed.piece == Piece::WhiteKing) {
            found_king = true;
        }
    }

    require(found_king, "man was not promoted after the move");
}

void test_rank_round_trip(Variant variant, unsigned pieces) {
    const auto count = entry_count(variant, pieces);

    // Full test for small tables; sampled endpoints for larger ones.
    const std::uint64_t limit = std::min<std::uint64_t>(count, 100000);

    for (std::uint64_t index = 0; index < limit; ++index) {
        const Position p = unrank_position(variant, pieces, index);
        require(rank_position(p) == index,
                "rank/unrank round trip failed");
    }

    if (count > 0) {
        const Position last =
            unrank_position(variant, pieces, count - 1);
        require(rank_position(last) == count - 1,
                "last rank/unrank entry failed");
    }
}

void test_international_geometry() {
    BoardGeometry g(Variant::International);
    require(g.board_size() == 10,
            "international board is not 10x10");
    require(g.playable_squares() == 50,
            "international board does not have 50 squares");
}

} // namespace

int main() {
    try {
        test_mandatory_capture();
        test_longest_capture_sequence();
        test_cannot_capture_same_piece_twice();
        test_flying_king_quiet_moves();
        test_flying_king_capture_landings();
        test_man_captures_backwards();
        test_promotion_at_end_of_move();

        test_rank_round_trip(Variant::Brazilian, 1);
        test_rank_round_trip(Variant::Brazilian, 2);
        test_rank_round_trip(Variant::International, 1);
        test_rank_round_trip(Variant::International, 2);

        test_international_geometry();

        std::cout << "All tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "TEST FAILURE: "
                  << exception.what() << '\n';
        return 1;
    }
}
