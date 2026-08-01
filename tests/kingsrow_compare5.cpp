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
#include "egdb/egdb_intl.h"
#include <thread>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace kr = egdb_interface;

namespace {

void message(const char* text) {
    if (text) std::cerr << "[Kingsrow] " << text << '\n';
}

std::string valueAfter(int& i, int argc, char** argv) {
    if (++i >= argc) {
        throw std::runtime_error("Missing argument value");
    }
    return argv[i];
}

kr::EGDB_BITBOARD kingsrowBits(dwtb::Bits source) {
    kr::EGDB_BITBOARD result = 0;

    while (source) {
        const int square = std::countr_zero(source);
        source &= source - 1;

        // Ghost bits occur after each group of ten squares.
        const int kingsrowBit = square + square / 10;
        result |= kr::EGDB_BITBOARD{1} << kingsrowBit;
    }

    return result;
}

dwtb::Value kingsrowOracle(
    kr::EGDB_DRIVER* handle,
    const dwtb::Rules& rules,
    const dwtb::Position& position,
    dwtb::Color side) {

    if (position.pieces(side) == 0) {
        return dwtb::Value::Loss;
    }

    if (position.pieces(dwtb::other(side)) == 0) {
        return dwtb::Value::Win;
    }

    /*
     * Kingsrow WLD v2 does not contain valid values for positions
     * where the side to move has a capture. Resolve those positions
     * through their mandatory maximum-capture children first.
     */
    if (rules.hasCapture(position, side)) {
        const auto moves = rules.captureMoves(position, side);

        bool sawDraw = false;

        for (const auto& move : moves) {
            const auto child = rules.apply(position, side, move);
            const auto value = kingsrowOracle(
                handle,
                rules,
                child,
                dwtb::other(side));

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

    if (rules.quietMoves(position, side).empty()) {
        return dwtb::Value::Loss;
    }

    kr::EGDB_POSITION external{
        kingsrowBits(position.black),
        kingsrowBits(position.white),
        kingsrowBits(position.kings)
    };

    const int externalSide =
        side == dwtb::Color::White
            ? kr::EGDB_WHITE
            : kr::EGDB_BLACK;

    const int result = kr::egdb_lookup(
        handle,
        &external,
        externalSide,
        0);

    if (result == kr::EGDB_WIN)  return dwtb::Value::Win;
    if (result == kr::EGDB_LOSS) return dwtb::Value::Loss;
    if (result == kr::EGDB_DRAW) return dwtb::Value::Draw;

    throw std::runtime_error(
        "Kingsrow returned unavailable/unknown value");
}

std::vector<dwtb::Material> allFivePieceMaterials() {
    std::vector<dwtb::Material> result;

    for (unsigned wm = 0; wm <= 5; ++wm) {
        for (unsigned wk = 0; wk <= 5 - wm; ++wk) {
            for (unsigned bm = 0; bm <= 5 - wm - wk; ++bm) {
                const unsigned bk = 5 - wm - wk - bm;

                if (wm + wk == 0 || bm + bk == 0) continue;

                result.push_back(dwtb::Material{
                    static_cast<std::uint8_t>(wm),
                    static_cast<std::uint8_t>(wk),
                    static_cast<std::uint8_t>(bm),
                    static_cast<std::uint8_t>(bk)
                });
            }
        }
    }

    return result;
}

} // namespace

int main(int argc, char** argv) {
    try {
        fs::path kingsrowDirectory;
        fs::path outputDirectory;
        unsigned threads = std::thread::hardware_concurrency();

        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];

            if (argument == "--kingsrow") {
                kingsrowDirectory =
                    valueAfter(i, argc, argv);
            } else if (argument == "--out") {
                outputDirectory =
                    valueAfter(i, argc, argv);
            } else if (argument == "--threads") {
                threads = static_cast<unsigned>(
                    std::stoul(valueAfter(i, argc, argv)));
            } else {
                throw std::runtime_error(
                    "Unknown argument: " + argument);
            }
        }

        if (kingsrowDirectory.empty() ||
            outputDirectory.empty()) {
            throw std::runtime_error(
                "Usage: kingsrow_compare5 "
                "--kingsrow DIR --out DIR [--threads N]");
        }

        dwtb::GenerateOptions generation;
        generation.outputDirectory = outputDirectory;
        generation.temporaryDirectory =
            outputDirectory / ".tmp";
        generation.boardSize = 10;
        generation.maxPieces = 5;
        generation.threads = std::max(1U, threads);
        generation.blockLog2 = 20;
        generation.zstdLevel = 19;
        generation.elideCaptures = true;

        dwtb::generateTablebases(generation);

        kr::EGDB_DRIVER* kingsrow = kr::egdb_open(
            "maxpieces=5",
            2048,
            kingsrowDirectory.string().c_str(),
            message);

        if (!kingsrow) {
            throw std::runtime_error(
                "Could not open Kingsrow database");
        }

        dwtb::Tablebase ours(outputDirectory, 10, 2048);
        dwtb::Rules rules(10);

        std::uint64_t checked = 0;

        for (const auto& material : allFivePieceMaterials()) {
            dwtb::Ranker ranker(10, material);

            std::cerr
                << "Comparing "
                << static_cast<unsigned>(material.whiteMen)
                << "wm "
                << static_cast<unsigned>(material.whiteKings)
                << "wk "
                << static_cast<unsigned>(material.blackMen)
                << "bm "
                << static_cast<unsigned>(material.blackKings)
                << "bk: "
                << ranker.positions()
                << " placements\n";

            for (std::uint64_t rank = 0;
                 rank < ranker.positions();
                 ++rank) {

                const auto position = ranker.unrank(rank);

                for (unsigned sideIndex = 0;
                     sideIndex < 2;
                     ++sideIndex) {

                    const auto side =
                        static_cast<dwtb::Color>(sideIndex);

                    const auto ourValue =
                        ours.probe(position, side);

                    const auto kingsrowValue =
                        kingsrowOracle(
                            kingsrow,
                            rules,
                            position,
                            side);

                    if (ourValue != kingsrowValue) {
                        std::cerr
                            << "Mismatch after " << checked
                            << " positions\n"
                            << "material rank: " << rank << '\n'
                            << "side: "
                            << (side == dwtb::Color::White
                                ? "white"
                                : "black")
                            << '\n'
                            << "white: 0x" << std::hex
                            << position.white << '\n'
                            << "black: 0x"
                            << position.black << '\n'
                            << "kings: 0x"
                            << position.kings << std::dec << '\n'
                            << "ours: "
                            << dwtb::valueName(ourValue) << '\n'
                            << "Kingsrow: "
                            << dwtb::valueName(kingsrowValue)
                            << '\n';

                        kr::egdb_close(kingsrow);
                        return 1;
                    }

                    ++checked;

                    if ((checked % 1'000'000) == 0) {
                        std::cerr
                            << "Checked " << checked
                            << " side-to-move positions\n";
                    }
                }
            }
        }

        kr::egdb_close(kingsrow);

        std::cout
            << "All " << checked
            << " five-piece side-to-move positions "
            << "match Kingsrow\n";

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "kingsrow_compare5: "
                  << e.what() << '\n';
        return 1;
    }
}
