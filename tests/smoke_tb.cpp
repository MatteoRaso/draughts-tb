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

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

#define CHECK(condition)                                             \
    do {                                                             \
        if (!(condition)) {                                          \
            throw std::runtime_error(                                \
                std::string("CHECK failed at line ") +               \
                std::to_string(__LINE__) + ": " #condition);         \
        }                                                            \
    } while (false)

dwtb::Value minimaxExpected(
    const dwtb::Rules& rules,
    const dwtb::Tablebase& tablebase,
    const dwtb::Position& p,
    dwtb::Color side) {

    const auto moves = rules.legalMoves(p, side);

    if (moves.empty()) {
        return dwtb::Value::Loss;
    }

    bool sawDraw = false;

    for (const auto& move : moves) {
        const auto child = rules.apply(p, side, move);
        const auto value =
            tablebase.probe(child, dwtb::other(side));

        CHECK(value != dwtb::Value::Unknown);

        if (value == dwtb::Value::Loss) {
            return dwtb::Value::Win;
        }
        if (value == dwtb::Value::Draw) {
            sawDraw = true;
        }
    }

    return sawDraw
        ? dwtb::Value::Draw
        : dwtb::Value::Loss;
}

} // namespace

int main() {
    try {
        const auto nonce =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();

        const fs::path directory =
            fs::temp_directory_path() /
            ("dwtb-smoke-" + std::to_string(nonce));

        dwtb::GenerateOptions options;
        options.outputDirectory = directory;
        options.temporaryDirectory = directory / ".tmp";
        options.boardSize = 8;
        options.maxPieces = 2;
        options.threads = 2;
        options.blockLog2 = 10;
        options.zstdLevel = 1;

        dwtb::generateTablebases(options);

        CHECK(fs::exists(directory / "manifest.json"));
        CHECK(fs::exists(directory / "manifest.json.sha256"));

        dwtb::Tablebase tablebase(directory, 8, 32);
        dwtb::Rules rules(8);

        for (unsigned whiteKing = 0;
             whiteKing <= 1;
             ++whiteKing) {
            for (unsigned blackKing = 0;
                 blackKing <= 1;
                 ++blackKing) {

                const dwtb::Material material{
                    static_cast<std::uint8_t>(!whiteKing),
                    static_cast<std::uint8_t>(whiteKing),
                    static_cast<std::uint8_t>(!blackKing),
                    static_cast<std::uint8_t>(blackKing)
                };

                dwtb::Ranker ranker(8, material);

                for (std::uint64_t rank = 0;
                     rank < ranker.positions();
                     ++rank) {

                    const auto position = ranker.unrank(rank);

                    for (unsigned sideIndex = 0;
                         sideIndex < 2;
                         ++sideIndex) {

                        const auto side =
                            static_cast<dwtb::Color>(sideIndex);

                        const auto actual =
                            tablebase.probe(position, side);

                        const auto expected =
                            minimaxExpected(
                                rules,
                                tablebase,
                                position,
                                side);

                        CHECK(actual == expected);
                    }
                }
            }
        }

        fs::remove_all(directory);

        std::cout << "Two-piece tablebase smoke test passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
