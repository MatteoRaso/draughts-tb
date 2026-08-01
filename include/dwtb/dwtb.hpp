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
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace dwtb {

using Bits = std::uint64_t;

enum class Color : std::uint8_t {
    White = 0,
    Black = 1
};

enum class Value : std::uint8_t {
    Loss    = 0,
    Draw    = 1,
    Win     = 2,
    Unknown = 3
};

inline Color other(Color c) {
    return c == Color::White ? Color::Black : Color::White;
}

inline Value invert(Value v) {
    if (v == Value::Win)  return Value::Loss;
    if (v == Value::Loss) return Value::Win;
    return v;
}

struct Position {
    Bits white = 0;
    Bits black = 0;
    Bits kings = 0;

    [[nodiscard]] Bits occupied() const {
        return white | black;
    }

    [[nodiscard]] Bits pieces(Color c) const {
        return c == Color::White ? white : black;
    }

    bool operator==(const Position&) const = default;
};

struct Material {
    std::uint8_t whiteMen   = 0;
    std::uint8_t whiteKings = 0;
    std::uint8_t blackMen   = 0;
    std::uint8_t blackKings = 0;

    [[nodiscard]] unsigned total() const {
        return whiteMen + whiteKings + blackMen + blackKings;
    }

    [[nodiscard]] unsigned totalKings() const {
        return whiteKings + blackKings;
    }

    bool operator==(const Material&) const = default;
};

bool operator<(const Material& a, const Material& b);

struct Move {
    std::uint8_t from = 0;
    std::uint8_t to = 0;

    // Ordered landing squares. Repeated squares are legal.
    std::vector<std::uint8_t> landings;

    // Ordered captured pieces. Every square must be unique.
    std::vector<std::uint8_t> captured;

    [[nodiscard]] bool isCapture() const {
        return !captured.empty();
    }
};

class Rules {
public:
    explicit Rules(int boardSize);

    [[nodiscard]] int boardSize() const;
    [[nodiscard]] int squareCount() const;

    // Returns -1 for a light square or an out-of-board coordinate.
    [[nodiscard]] int square(int row, int column) const;
    [[nodiscard]] int row(int square) const;
    [[nodiscard]] int column(int square) const;

    [[nodiscard]] bool isPromotionSquare(Color c, int square) const;

    [[nodiscard]] bool hasCapture(
        const Position& position,
        Color sideToMove) const;

    // Returns only globally maximum-length capture sequences.
    [[nodiscard]] std::vector<Move> captureMoves(
        const Position& position,
        Color sideToMove) const;

    // Does not check mandatory capture. Intended for generator internals.
    [[nodiscard]] std::vector<Move> quietMoves(
        const Position& position,
        Color sideToMove) const;

    [[nodiscard]] std::vector<Move> legalMoves(
        const Position& position,
        Color sideToMove) const;

    [[nodiscard]] Position apply(
        const Position& position,
        Color sideToMove,
        const Move& move) const;

    // Reverse non-promoting quiet moves in the same material slice.
    [[nodiscard]] std::vector<Position> quietPredecessors(
        const Position& child,
        Color parentSideToMove) const;

private:
    int boardSize_;
    int squareCount_;
};

[[nodiscard]] Material materialOf(const Position& p);
[[nodiscard]] Material swappedMaterial(const Material& m);
[[nodiscard]] bool isCanonicalMaterial(const Material& m);

[[nodiscard]] Position rotate180AndSwapColors(
    const Position& p,
    int squareCount);

[[nodiscard]] std::string sliceFilename(
    int boardSize,
    const Material& material);

class Ranker {
public:
    Ranker(int boardSize, Material material);

    [[nodiscard]] std::uint64_t positions() const;
    [[nodiscard]] const Material& material() const;

    [[nodiscard]] bool valid(const Position& p) const;
    [[nodiscard]] std::uint64_t rank(const Position& p) const;
    [[nodiscard]] Position unrank(std::uint64_t rank) const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

class Tablebase {
public:
    Tablebase(
        std::filesystem::path directory,
        int boardSize,
        std::size_t cacheMiB = 512);

    [[nodiscard]] Value probe(
        const Position& position,
        Color sideToMove) const;

    [[nodiscard]] int boardSize() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

struct GenerateOptions {
    std::filesystem::path outputDirectory;
    std::filesystem::path temporaryDirectory;

    int boardSize = 8;
    int maxPieces = 5;
    unsigned threads = 0;

    // Number of logical positions in an independently compressed block.
    unsigned blockLog2 = 20;

    // Zstandard level. 19 is compression-oriented.
    int zstdLevel = 19;

    // Capture positions are solved dynamically by the driver and replaced
    // with block-local filler values before compression.
    bool elideCaptures = true;

    bool keepTemporaryFiles = false;
    bool force = false;
};

void generateTablebases(const GenerateOptions& options);

[[nodiscard]] std::string valueName(Value value);
[[nodiscard]] std::string sha256File(const std::filesystem::path& path);

} // namespace dwtb
