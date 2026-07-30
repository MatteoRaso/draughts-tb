/*
 This file is part of draughts-tb.

draughts-tb is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

draughts-tb is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with draughts-tb. If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace draughts {

enum class Variant : std::uint8_t {
    Brazilian = 0,
    International = 1
};

enum class Side : std::uint8_t {
    White = 0,
    Black = 1
};

enum class Piece : std::uint8_t {
    WhiteMan  = 0,
    WhiteKing = 1,
    BlackMan  = 2,
    BlackKing = 3
};

enum class WLD : std::uint8_t {
    Loss    = 0,
    Draw    = 1,
    Win     = 2,
    Invalid = 3
};

struct Rules {
    Variant variant = Variant::Brazilian;
    int board_size = 8;
    int playable_squares = 32;

    static Rules for_variant(Variant variant);
};

struct PlacedPiece {
    std::uint8_t square = 0; // Zero-based playable-square number.
    Piece piece = Piece::WhiteMan;

    friend bool operator==(const PlacedPiece&, const PlacedPiece&) = default;
};

struct Position {
    Variant variant = Variant::Brazilian;
    Side side_to_move = Side::White;
    std::vector<PlacedPiece> pieces;
};

struct Move {
    std::uint8_t from = 0;
    std::uint8_t to = 0;
    std::uint64_t captured = 0;

    [[nodiscard]] unsigned capture_count() const;
    [[nodiscard]] bool is_capture() const { return captured != 0; }

    friend bool operator==(const Move&, const Move&) = default;
};

class BoardGeometry {
public:
    explicit BoardGeometry(Variant variant);

    [[nodiscard]] int board_size() const { return rules_.board_size; }
    [[nodiscard]] int playable_squares() const {
        return rules_.playable_squares;
    }

    [[nodiscard]] int square_at(int row, int column) const;
    [[nodiscard]] std::pair<int, int> coordinates(int square) const;

private:
    Rules rules_;
    std::array<std::array<int, 10>, 10> square_{};
    std::array<std::pair<int, int>, 50> coordinates_{};
};

[[nodiscard]] bool is_white(Piece piece);
[[nodiscard]] bool is_king(Piece piece);
[[nodiscard]] Side opposite(Side side);

[[nodiscard]] std::vector<Move> legal_moves(const Position& position);
[[nodiscard]] Position apply_move(const Position& position, const Move& move);
[[nodiscard]] bool structurally_valid(const Position& position);

[[nodiscard]] std::uint64_t choose_u64(unsigned n, unsigned k);
[[nodiscard]] std::uint64_t entry_count(Variant variant, unsigned pieces);
[[nodiscard]] std::uint64_t rank_position(const Position& position);
[[nodiscard]] Position unrank_position(
    Variant variant,
    unsigned pieces,
    std::uint64_t index
);

class Tablebase {
public:
    explicit Tablebase(const std::filesystem::path& file);

    [[nodiscard]] Variant variant() const { return variant_; }
    [[nodiscard]] unsigned piece_count() const { return pieces_; }
    [[nodiscard]] std::uint64_t entries() const { return entries_; }

    [[nodiscard]] WLD at_index(std::uint64_t index) const;
    [[nodiscard]] WLD probe(const Position& position) const;

private:
    Variant variant_{};
    unsigned pieces_ = 0;
    std::uint64_t entries_ = 0;
    std::vector<std::uint8_t> packed_;
};

struct GenerateOptions {
    Variant variant = Variant::Brazilian;
    unsigned maximum_pieces = 4;
    unsigned threads = 0; // Zero means hardware_concurrency().
    std::filesystem::path output_prefix = "draughts";
    bool keep_scratch = false;
};

void generate_tablebases(const GenerateOptions& options);

[[nodiscard]] std::string sha256_file(
    const std::filesystem::path& file
);

[[nodiscard]] const char* wld_name(WLD value);

} // namespace draughts
