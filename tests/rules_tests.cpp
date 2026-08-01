/*
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
*/
#include "dwtb/dwtb.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace {

#define CHECK(condition)                                             \
    do {                                                             \
        if (!(condition)) {                                          \
            throw std::runtime_error(                                \
                std::string("CHECK failed at line ") +               \
                std::to_string(__LINE__) + ": " #condition);         \
        }                                                            \
    } while (false)

void add(
    dwtb::Position& p,
    dwtb::Color color,
    bool king,
    int square) {

    const dwtb::Bits b = dwtb::Bits{1} << square;

    if (color == dwtb::Color::White) p.white |= b;
    else                             p.black |= b;

    if (king) p.kings |= b;
}

int sq(const dwtb::Rules& rules, int row, int col) {
    const int result = rules.square(row, col);
    CHECK(result >= 0);
    return result;
}

void mandatoryCapture() {
    dwtb::Rules rules(8);
    dwtb::Position p;

    add(p, dwtb::Color::White, false, sq(rules, 5, 0));
    add(p, dwtb::Color::White, false, sq(rules, 5, 4));
    add(p, dwtb::Color::Black, false, sq(rules, 4, 1));

    const auto moves =
        rules.legalMoves(p, dwtb::Color::White);

    CHECK(!moves.empty());
    CHECK(std::all_of(
        moves.begin(),
        moves.end(),
        [](const dwtb::Move& move) {
            return move.isCapture();
        }));
}

void longestCaptureOnly() {
    dwtb::Rules rules(8);
    dwtb::Position p;

    const int longPiece = sq(rules, 5, 0);
    const int shortPiece = sq(rules, 5, 4);

    add(p, dwtb::Color::White, false, longPiece);
    add(p, dwtb::Color::White, false, shortPiece);

    add(p, dwtb::Color::Black, false, sq(rules, 4, 1));
    add(p, dwtb::Color::Black, false, sq(rules, 2, 3));
    add(p, dwtb::Color::Black, false, sq(rules, 4, 5));

    const auto moves =
        rules.legalMoves(p, dwtb::Color::White);

    CHECK(!moves.empty());

    for (const auto& move : moves) {
        CHECK(move.from == longPiece);
        CHECK(move.captured.size() == 2);
    }
}

void perfectLoopAndNoDoubleCapture() {
    dwtb::Rules rules(8);
    dwtb::Position p;

    const int start = sq(rules, 3, 2);

    add(p, dwtb::Color::White, true, start);

    add(p, dwtb::Color::Black, false, sq(rules, 2, 3));
    add(p, dwtb::Color::Black, false, sq(rules, 2, 5));
    add(p, dwtb::Color::Black, false, sq(rules, 4, 5));
    add(p, dwtb::Color::Black, false, sq(rules, 4, 3));

    const auto moves =
        rules.captureMoves(p, dwtb::Color::White);

    bool foundLoop = false;

    for (const auto& move : moves) {
        std::set<int> captured(
            move.captured.begin(),
            move.captured.end());

        CHECK(captured.size() == move.captured.size());

        if (move.to == start &&
            move.captured.size() == 4) {
            foundLoop = true;

            CHECK(std::count(
                move.landings.begin(),
                move.landings.end(),
                static_cast<std::uint8_t>(start)) >= 1);
        }
    }

    CHECK(foundLoop);
}

void flyingKing() {
    dwtb::Rules rules(8);
    dwtb::Position p;

    add(p, dwtb::Color::White, true, sq(rules, 7, 0));
    add(p, dwtb::Color::Black, false, sq(rules, 4, 3));

    const auto moves =
        rules.captureMoves(p, dwtb::Color::White);

    std::set<int> destinations;
    for (const auto& move : moves) {
        destinations.insert(move.to);
    }

    CHECK(destinations.count(sq(rules, 3, 4)) == 1);
    CHECK(destinations.count(sq(rules, 2, 5)) == 1);
    CHECK(destinations.count(sq(rules, 1, 6)) == 1);
    CHECK(destinations.count(sq(rules, 0, 7)) == 1);
}

void promoteOnlyAtEndOfTurn() {
    dwtb::Rules rules(8);
    dwtb::Position p;

    const int start = sq(rules, 2, 1);
    const int end = sq(rules, 2, 5);

    add(p, dwtb::Color::White, false, start);
    add(p, dwtb::Color::Black, false, sq(rules, 1, 2));
    add(p, dwtb::Color::Black, false, sq(rules, 1, 4));

    const auto moves =
        rules.captureMoves(p, dwtb::Color::White);

    CHECK(moves.size() == 1);
    CHECK(moves.front().captured.size() == 2);
    CHECK(moves.front().to == end);

    const auto result =
        rules.apply(p, dwtb::Color::White, moves.front());

    CHECK((result.white & (dwtb::Bits{1} << end)) != 0);
    CHECK((result.kings & (dwtb::Bits{1} << end)) == 0);

    // Also verify promotion when the turn really ends on the back rank.
    dwtb::Position promote;
    add(promote, dwtb::Color::White, false, start);
    add(promote, dwtb::Color::Black, false, sq(rules, 1, 2));

    const auto promotionMoves =
        rules.captureMoves(promote, dwtb::Color::White);

    CHECK(!promotionMoves.empty());

    const auto promoted =
        rules.apply(
            promote,
            dwtb::Color::White,
            promotionMoves.front());

    CHECK((promoted.kings &
           (dwtb::Bits{1} << promotionMoves.front().to)) != 0);
}

void menCaptureBackwards() {
    dwtb::Rules rules(8);
    dwtb::Position p;

    const int from = sq(rules, 3, 2);
    const int to = sq(rules, 5, 4);

    add(p, dwtb::Color::White, false, from);
    add(p, dwtb::Color::Black, false, sq(rules, 4, 3));

    const auto moves =
        rules.captureMoves(p, dwtb::Color::White);

    CHECK(moves.size() == 1);
    CHECK(moves.front().from == from);
    CHECK(moves.front().to == to);
}

void rankerRoundTrip() {
    for (int board : {8, 10}) {
        const dwtb::Material material{2, 1, 1, 1};
        const dwtb::Ranker ranker(board, material);

        const std::uint64_t step =
            std::max<std::uint64_t>(
                1,
                ranker.positions() / 1000);

        for (std::uint64_t rank = 0;
             rank < ranker.positions();
             rank += step) {

            const auto position = ranker.unrank(rank);
            CHECK(ranker.valid(position));
            CHECK(ranker.rank(position) == rank);
        }
    }
}

} // namespace

int main() {
    try {
        mandatoryCapture();
        longestCaptureOnly();
        perfectLoopAndNoDoubleCapture();
        flyingKing();
        promoteOnlyAtEndOfTurn();
        menCaptureBackwards();
        rankerRoundTrip();

        std::cout << "All rule tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
