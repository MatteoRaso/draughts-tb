#include "wld.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include <openssl/evp.h>

namespace draughts {
namespace {

constexpr std::uint8_t UNKNOWN = 255;
constexpr std::size_t HEADER_SIZE = 64;

#pragma pack(push, 1)
struct FileHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t board_size;
    std::uint32_t pieces;
    std::uint32_t playable_squares;
    std::uint64_t entries;
    std::uint64_t data_offset;
    std::uint8_t variant;
    std::uint8_t reserved[23];
};
#pragma pack(pop)

static_assert(sizeof(FileHeader) == HEADER_SIZE);

bool belongs_to(Piece piece, Side side) {
    return side == Side::White ? is_white(piece) : !is_white(piece);
}

std::uint64_t checked_multiply(std::uint64_t a, std::uint64_t b) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
        throw std::overflow_error("tablebase entry count exceeds uint64_t");
    }
    return a * b;
}

std::uint64_t four_power(unsigned n) {
    std::uint64_t result = 1;
    for (unsigned i = 0; i < n; ++i) {
        result = checked_multiply(result, 4);
    }
    return result;
}

std::uint64_t occupancy_rank(const std::vector<PlacedPiece>& pieces) {
    std::uint64_t rank = 0;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        rank += choose_u64(pieces[i].square, static_cast<unsigned>(i + 1));
    }
    return rank;
}

std::vector<unsigned> unrank_occupancy(
    unsigned squares,
    unsigned pieces,
    std::uint64_t rank
) {
    std::vector<unsigned> result(pieces);
    unsigned limit = squares;

    for (unsigned i = pieces; i > 0; --i) {
        unsigned x = limit - 1;
        while (choose_u64(x, i) > rank) {
            if (x == 0) {
                throw std::runtime_error("invalid combinadic rank");
            }
            --x;
        }

        result[i - 1] = x;
        rank -= choose_u64(x, i);
        limit = x;
    }

    if (rank != 0) {
        throw std::runtime_error("invalid residual combinadic rank");
    }

    return result;
}

std::array<int, 50> make_piece_array(const Position& position) {
    std::array<int, 50> board;
    board.fill(-1);

    for (const auto& placed : position.pieces) {
        if (placed.square >= board.size() || board[placed.square] != -1) {
            throw std::invalid_argument("duplicate or out-of-range square");
        }
        board[placed.square] = static_cast<int>(placed.piece);
    }

    return board;
}

bool empty_during_capture(
    int square,
    int current,
    int original_start,
    const std::array<int, 50>& board
) {
    if (square == current) {
        return false;
    }
    if (square == original_start) {
        return true;
    }
    return board[square] < 0;
}

int piece_during_capture(
    int square,
    int current,
    int original_start,
    Piece moving_piece,
    const std::array<int, 50>& board
) {
    if (square == current) {
        return static_cast<int>(moving_piece);
    }
    if (square == original_start) {
        return -1;
    }
    return board[square];
}

void generate_piece_captures(
    const Position& position,
    const BoardGeometry& geometry,
    const std::array<int, 50>& board,
    int start,
    Piece moving_piece,
    int current,
    std::uint64_t captured,
    std::vector<Move>& output
) {
    static constexpr int directions[4][2] = {
        {-1, -1}, {-1, 1}, {1, -1}, {1, 1}
    };

    const auto [row, column] = geometry.coordinates(current);
    bool found = false;

    if (!is_king(moving_piece)) {
        for (const auto& direction : directions) {
            const int middle = geometry.square_at(
                row + direction[0],
                column + direction[1]
            );
            const int landing = geometry.square_at(
                row + 2 * direction[0],
                column + 2 * direction[1]
            );

            if (middle < 0 || landing < 0) {
                continue;
            }

            const int middle_piece = piece_during_capture(
                middle, current, start, moving_piece, board
            );

            if (middle_piece < 0 ||
                belongs_to(static_cast<Piece>(middle_piece),
                           is_white(moving_piece)
                               ? Side::White
                               : Side::Black)) {
                continue;
            }

            const std::uint64_t middle_bit = std::uint64_t{1} << middle;
            if ((captured & middle_bit) != 0) {
                continue;
            }

            if (!empty_during_capture(
                    landing, current, start, board)) {
                continue;
            }

            found = true;
            generate_piece_captures(
                position,
                geometry,
                board,
                start,
                moving_piece,
                landing,
                captured | middle_bit,
                output
            );
        }
    } else {
        for (const auto& direction : directions) {
            int scan_row = row + direction[0];
            int scan_column = column + direction[1];

            while (true) {
                const int square =
                    geometry.square_at(scan_row, scan_column);
                if (square < 0) {
                    break;
                }

                if (!empty_during_capture(
                        square, current, start, board)) {
                    break;
                }

                scan_row += direction[0];
                scan_column += direction[1];
            }

            const int victim =
                geometry.square_at(scan_row, scan_column);
            if (victim < 0) {
                continue;
            }

            const int victim_piece = piece_during_capture(
                victim, current, start, moving_piece, board
            );

            if (victim_piece < 0 ||
                belongs_to(static_cast<Piece>(victim_piece),
                           is_white(moving_piece)
                               ? Side::White
                               : Side::Black)) {
                continue;
            }

            const std::uint64_t victim_bit = std::uint64_t{1} << victim;

            // A previously captured piece remains on the board as a blocker
            // until the entire sequence has ended.
            if ((captured & victim_bit) != 0) {
                continue;
            }

            int landing_row = scan_row + direction[0];
            int landing_column = scan_column + direction[1];

            while (true) {
                const int landing =
                    geometry.square_at(landing_row, landing_column);
                if (landing < 0 ||
                    !empty_during_capture(
                        landing, current, start, board)) {
                    break;
                }

                found = true;
                generate_piece_captures(
                    position,
                    geometry,
                    board,
                    start,
                    moving_piece,
                    landing,
                    captured | victim_bit,
                    output
                );

                landing_row += direction[0];
                landing_column += direction[1];
            }
        }
    }

    if (!found && captured != 0) {
        output.push_back(Move{
            static_cast<std::uint8_t>(start),
            static_cast<std::uint8_t>(current),
            captured
        });
    }
}

std::vector<Move> captures_for_piece(
    const Position& position,
    int start,
    Piece piece,
    const BoardGeometry& geometry,
    const std::array<int, 50>& board
) {
    std::vector<Move> result;

    generate_piece_captures(
        position,
        geometry,
        board,
        start,
        piece,
        start,
        0,
        result
    );

    std::sort(result.begin(), result.end(), [](const Move& a, const Move& b) {
        if (a.from != b.from) return a.from < b.from;
        if (a.to != b.to) return a.to < b.to;
        return a.captured < b.captured;
    });
    result.erase(std::unique(result.begin(), result.end()), result.end());

    return result;
}

std::vector<Move> quiets_for_piece(
    int start,
    Piece piece,
    const BoardGeometry& geometry,
    const std::array<int, 50>& board
) {
    std::vector<Move> result;
    const auto [row, column] = geometry.coordinates(start);

    if (!is_king(piece)) {
        const int row_delta = is_white(piece) ? -1 : 1;

        for (int column_delta : {-1, 1}) {
            const int target =
                geometry.square_at(
                    row + row_delta,
                    column + column_delta
                );

            if (target >= 0 && board[target] < 0) {
                result.push_back(Move{
                    static_cast<std::uint8_t>(start),
                    static_cast<std::uint8_t>(target),
                    0
                });
            }
        }
        return result;
    }

    static constexpr int directions[4][2] = {
        {-1, -1}, {-1, 1}, {1, -1}, {1, 1}
    };

    for (const auto& direction : directions) {
        int scan_row = row + direction[0];
        int scan_column = column + direction[1];

        while (true) {
            const int target =
                geometry.square_at(scan_row, scan_column);

            if (target < 0 || board[target] >= 0) {
                break;
            }

            result.push_back(Move{
                static_cast<std::uint8_t>(start),
                static_cast<std::uint8_t>(target),
                0
            });

            scan_row += direction[0];
            scan_column += direction[1];
        }
    }

    return result;
}

template<class Function>
void parallel_ranges(
    std::uint64_t count,
    unsigned thread_count,
    Function function
) {
    if (thread_count == 0) {
        thread_count = std::thread::hardware_concurrency();
    }
    if (thread_count == 0) {
        thread_count = 1;
    }

    thread_count = static_cast<unsigned>(
        std::min<std::uint64_t>(thread_count, std::max<std::uint64_t>(1, count))
    );

    std::vector<std::jthread> workers;
    workers.reserve(thread_count);

    for (unsigned thread = 0; thread < thread_count; ++thread) {
        const std::uint64_t begin = count * thread / thread_count;
        const std::uint64_t end = count * (thread + 1) / thread_count;

        workers.emplace_back([=, &function] {
            function(begin, end);
        });
    }
}

std::filesystem::path tablebase_name(
    const std::filesystem::path& prefix,
    unsigned pieces
) {
    return prefix.string() + "." + std::to_string(pieces) + ".wld";
}

void write_sha_sidecar(const std::filesystem::path& file) {
    const std::string digest = sha256_file(file);
    const auto sidecar = file.string() + ".sha256";

    std::ofstream output(sidecar, std::ios::binary);
    if (!output) {
        throw std::runtime_error("cannot write SHA-256 sidecar");
    }

    output << digest << "  " << file.filename().string() << '\n';
}

} // namespace

Rules Rules::for_variant(Variant variant) {
    if (variant == Variant::Brazilian) {
        return Rules{variant, 8, 32};
    }
    return Rules{variant, 10, 50};
}

unsigned Move::capture_count() const {
    return std::popcount(captured);
}

BoardGeometry::BoardGeometry(Variant variant)
    : rules_(Rules::for_variant(variant)) {
    for (auto& row : square_) {
        row.fill(-1);
    }

    int square = 0;
    for (int row = 0; row < rules_.board_size; ++row) {
        for (int column = 0; column < rules_.board_size; ++column) {
            if ((row + column) % 2 == 1) {
                square_[row][column] = square;
                coordinates_[square] = {row, column};
                ++square;
            }
        }
    }
}

int BoardGeometry::square_at(int row, int column) const {
    if (row < 0 || column < 0 ||
        row >= rules_.board_size || column >= rules_.board_size) {
        return -1;
    }
    return square_[row][column];
}

std::pair<int, int> BoardGeometry::coordinates(int square) const {
    if (square < 0 || square >= rules_.playable_squares) {
        throw std::out_of_range("playable square out of range");
    }
    return coordinates_[square];
}

bool is_white(Piece piece) {
    return piece == Piece::WhiteMan ||
           piece == Piece::WhiteKing;
}

bool is_king(Piece piece) {
    return piece == Piece::WhiteKing ||
           piece == Piece::BlackKing;
}

Side opposite(Side side) {
    return side == Side::White ? Side::Black : Side::White;
}

bool structurally_valid(const Position& position) {
    const Rules rules = Rules::for_variant(position.variant);
    BoardGeometry geometry(position.variant);

    std::uint64_t occupied = 0;

    for (const auto& placed : position.pieces) {
        if (placed.square >= rules.playable_squares) {
            return false;
        }

        const std::uint64_t bit = std::uint64_t{1} << placed.square;
        if ((occupied & bit) != 0) {
            return false;
        }
        occupied |= bit;

        const auto [row, column] =
            geometry.coordinates(placed.square);
        (void)column;

        // Men cannot remain unpromoted on their promotion row.
        if (placed.piece == Piece::WhiteMan && row == 0) {
            return false;
        }
        if (placed.piece == Piece::BlackMan &&
            row == rules.board_size - 1) {
            return false;
        }
    }

    return true;
}

std::vector<Move> legal_moves(const Position& position) {
    if (!structurally_valid(position)) {
        return {};
    }

    const BoardGeometry geometry(position.variant);
    const auto board = make_piece_array(position);

    std::vector<Move> captures;
    std::vector<Move> quiets;

    for (const auto& placed : position.pieces) {
        if (!belongs_to(placed.piece, position.side_to_move)) {
            continue;
        }

        auto piece_captures = captures_for_piece(
            position,
            placed.square,
            placed.piece,
            geometry,
            board
        );

        captures.insert(
            captures.end(),
            piece_captures.begin(),
            piece_captures.end()
        );

        auto piece_quiets = quiets_for_piece(
            placed.square,
            placed.piece,
            geometry,
            board
        );

        quiets.insert(
            quiets.end(),
            piece_quiets.begin(),
            piece_quiets.end()
        );
    }

    if (!captures.empty()) {
        unsigned maximum = 0;
        for (const auto& move : captures) {
            maximum = std::max(maximum, move.capture_count());
        }

        captures.erase(
            std::remove_if(
                captures.begin(),
                captures.end(),
                [maximum](const Move& move) {
                    return move.capture_count() != maximum;
                }
            ),
            captures.end()
        );

        return captures;
    }

    return quiets;
}

Position apply_move(const Position& position, const Move& move) {
    Position result = position;
    result.side_to_move = opposite(position.side_to_move);

    Piece moving_piece{};
    bool found = false;

    result.pieces.erase(
        std::remove_if(
            result.pieces.begin(),
            result.pieces.end(),
            [&](const PlacedPiece& placed) {
                if (placed.square == move.from) {
                    moving_piece = placed.piece;
                    found = true;
                    return true;
                }

                return (move.captured &
                        (std::uint64_t{1} << placed.square)) != 0;
            }
        ),
        result.pieces.end()
    );

    if (!found) {
        throw std::invalid_argument("move source is empty");
    }

    const BoardGeometry geometry(position.variant);
    const Rules rules = Rules::for_variant(position.variant);
    const auto [row, column] = geometry.coordinates(move.to);
    (void)column;

    if (moving_piece == Piece::WhiteMan && row == 0) {
        moving_piece = Piece::WhiteKing;
    } else if (moving_piece == Piece::BlackMan &&
               row == rules.board_size - 1) {
        moving_piece = Piece::BlackKing;
    }

    result.pieces.push_back(PlacedPiece{move.to, moving_piece});
    std::sort(
        result.pieces.begin(),
        result.pieces.end(),
        [](const PlacedPiece& a, const PlacedPiece& b) {
            return a.square < b.square;
        }
    );

    return result;
}

std::uint64_t choose_u64(unsigned n, unsigned k) {
    if (k > n) {
        return 0;
    }
    if (k == 0 || k == n) {
        return 1;
    }

    k = std::min(k, n - k);

    unsigned __int128 result = 1;
    for (unsigned i = 1; i <= k; ++i) {
        result = result * (n - k + i) / i;
        if (result > std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("binomial coefficient overflow");
        }
    }

    return static_cast<std::uint64_t>(result);
}

std::uint64_t entry_count(Variant variant, unsigned pieces) {
    const auto rules = Rules::for_variant(variant);

    std::uint64_t count =
        choose_u64(rules.playable_squares, pieces);
    count = checked_multiply(count, four_power(pieces));
    count = checked_multiply(count, 2);
    return count;
}

std::uint64_t rank_position(const Position& input) {
    Position position = input;

    std::sort(
        position.pieces.begin(),
        position.pieces.end(),
        [](const PlacedPiece& a, const PlacedPiece& b) {
            return a.square < b.square;
        }
    );

    const Rules rules = Rules::for_variant(position.variant);
    if (position.pieces.size() > static_cast<std::size_t>(
            rules.playable_squares)) {
        throw std::invalid_argument("too many pieces");
    }

    for (std::size_t i = 1; i < position.pieces.size(); ++i) {
        if (position.pieces[i - 1].square ==
            position.pieces[i].square) {
            throw std::invalid_argument("duplicate occupied square");
        }
    }

    std::uint64_t type_code = 0;
    std::uint64_t multiplier = 1;

    for (const auto& placed : position.pieces) {
        type_code += multiplier *
            static_cast<std::uint8_t>(placed.piece);
        multiplier *= 4;
    }

    const std::uint64_t combination =
        occupancy_rank(position.pieces);

    const std::uint64_t side =
        position.side_to_move == Side::White ? 0 : 1;

    return ((combination * four_power(
                static_cast<unsigned>(position.pieces.size()))
            + type_code) * 2) + side;
}

Position unrank_position(
    Variant variant,
    unsigned pieces,
    std::uint64_t index
) {
    if (index >= entry_count(variant, pieces)) {
        throw std::out_of_range("tablebase index out of range");
    }

    Position position;
    position.variant = variant;
    position.side_to_move =
        (index & 1) == 0 ? Side::White : Side::Black;

    std::uint64_t value = index / 2;
    const std::uint64_t type_base = four_power(pieces);
    const std::uint64_t type_code_initial = value % type_base;
    const std::uint64_t combination_rank = value / type_base;

    const Rules rules = Rules::for_variant(variant);
    const auto occupied = unrank_occupancy(
        rules.playable_squares,
        pieces,
        combination_rank
    );

    std::uint64_t type_code = type_code_initial;

    for (unsigned i = 0; i < pieces; ++i) {
        position.pieces.push_back(PlacedPiece{
            static_cast<std::uint8_t>(occupied[i]),
            static_cast<Piece>(type_code & 3)
        });
        type_code >>= 2;
    }

    return position;
}

Tablebase::Tablebase(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open tablebase: " + file.string());
    }

    FileHeader header{};
    input.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (!input ||
        std::memcmp(header.magic, "WLDDB1", 6) != 0 ||
        header.version != 1 ||
        header.data_offset != HEADER_SIZE) {
        throw std::runtime_error("invalid tablebase header");
    }

    variant_ = static_cast<Variant>(header.variant);
    pieces_ = header.pieces;
    entries_ = header.entries;

    const std::uint64_t packed_size = (entries_ + 3) / 4;
    packed_.resize(static_cast<std::size_t>(packed_size));

    input.seekg(static_cast<std::streamoff>(header.data_offset));
    input.read(
        reinterpret_cast<char*>(packed_.data()),
        static_cast<std::streamsize>(packed_.size())
    );

    if (!input) {
        throw std::runtime_error("truncated tablebase");
    }
}

WLD Tablebase::at_index(std::uint64_t index) const {
    if (index >= entries_) {
        throw std::out_of_range("tablebase index out of range");
    }

    const std::uint8_t byte = packed_[index / 4];
    const unsigned shift = static_cast<unsigned>((index % 4) * 2);
    return static_cast<WLD>((byte >> shift) & 3);
}

WLD Tablebase::probe(const Position& position) const {
    if (position.variant != variant_ ||
        position.pieces.size() != pieces_) {
        throw std::invalid_argument("position/tablebase mismatch");
    }

    if (!structurally_valid(position)) {
        return WLD::Invalid;
    }

    return at_index(rank_position(position));
}

std::string sha256_file(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open file for SHA-256");
    }

    EVP_MD_CTX* raw = EVP_MD_CTX_new();
    if (!raw) {
        throw std::runtime_error("EVP_MD_CTX_new failed");
    }

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>
        context(raw, EVP_MD_CTX_free);

    if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("EVP_DigestInit_ex failed");
    }

    std::array<char, 1 << 20> buffer{};

    while (input) {
        input.read(buffer.data(), buffer.size());
        const auto count = input.gcount();

        if (count > 0 &&
            EVP_DigestUpdate(
                context.get(),
                buffer.data(),
                static_cast<std::size_t>(count)
            ) != 1) {
            throw std::runtime_error("EVP_DigestUpdate failed");
        }
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned digest_length = 0;

    if (EVP_DigestFinal_ex(
            context.get(), digest, &digest_length) != 1) {
        throw std::runtime_error("EVP_DigestFinal_ex failed");
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');

    for (unsigned i = 0; i < digest_length; ++i) {
        output << std::setw(2) << static_cast<unsigned>(digest[i]);
    }

    return output.str();
}

const char* wld_name(WLD value) {
    switch (value) {
        case WLD::Loss: return "loss";
        case WLD::Draw: return "draw";
        case WLD::Win: return "win";
        case WLD::Invalid: return "invalid";
    }
    return "unknown";
}

void generate_tablebases(const GenerateOptions& options) {
    if (options.maximum_pieces == 0) {
        throw std::invalid_argument("maximum_pieces must be positive");
    }

    unsigned threads = options.threads;
    if (threads == 0) {
        threads = std::thread::hardware_concurrency();
    }
    if (threads == 0) {
        threads = 1;
    }

    std::vector<std::unique_ptr<Tablebase>> lower(
        options.maximum_pieces + 1
    );

    for (unsigned pieces = 1;
         pieces <= options.maximum_pieces;
         ++pieces) {
        const std::uint64_t entries =
            entry_count(options.variant, pieces);

        if (entries >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
            throw std::runtime_error(
                "table does not fit this process's address space"
            );
        }

        std::cerr
            << "Generating " << pieces << "-piece table: "
            << entries << " entries, "
            << ((entries + 3) / 4) << " packed bytes\n";

        // Build representation uses one byte per entry so that atomic byte
        // reads and writes are possible. Final files use two bits per entry.
        std::vector<std::uint8_t> values(
            static_cast<std::size_t>(entries),
            UNKNOWN
        );

        // Initial classification.
        parallel_ranges(entries, threads,
            [&](std::uint64_t begin, std::uint64_t end) {
                for (std::uint64_t index = begin; index < end; ++index) {
                    Position position = unrank_position(
                        options.variant, pieces, index
                    );

                    if (!structurally_valid(position)) {
                        values[index] =
                            static_cast<std::uint8_t>(WLD::Invalid);
                        continue;
                    }

                    unsigned white = 0;
                    unsigned black = 0;

                    for (const auto& placed : position.pieces) {
                        if (is_white(placed.piece)) {
                            ++white;
                        } else {
                            ++black;
                        }
                    }

                    const unsigned own =
                        position.side_to_move == Side::White
                            ? white : black;
                    const unsigned opponent =
                        position.side_to_move == Side::White
                            ? black : white;

                    if (own == 0) {
                        values[index] =
                            static_cast<std::uint8_t>(WLD::Loss);
                        continue;
                    }

                    if (opponent == 0) {
                        values[index] =
                            static_cast<std::uint8_t>(WLD::Win);
                        continue;
                    }

                    if (legal_moves(position).empty()) {
                        values[index] =
                            static_cast<std::uint8_t>(WLD::Loss);
                    }
                }
            }
        );

        unsigned iteration = 0;

        while (true) {
            ++iteration;
            std::atomic<std::uint64_t> changed{0};

            parallel_ranges(entries, threads,
                [&](std::uint64_t begin, std::uint64_t end) {
                    for (std::uint64_t index = begin;
                         index < end;
                         ++index) {
                        std::atomic_ref<std::uint8_t> current(
                            values[index]
                        );

                        if (current.load(std::memory_order_relaxed)
                            != UNKNOWN) {
                            continue;
                        }

                        const Position position = unrank_position(
                            options.variant, pieces, index
                        );
                        const auto moves = legal_moves(position);

                        bool has_loss_successor = false;
                        bool all_successors_win = !moves.empty();

                        for (const Move& move : moves) {
                            const Position successor =
                                apply_move(position, move);

                            WLD successor_value;

                            if (successor.pieces.size() < pieces) {
                                const unsigned successor_count =
                                    static_cast<unsigned>(
                                        successor.pieces.size()
                                    );

                                if (successor_count == 0) {
                                    successor_value = WLD::Loss;
                                } else {
                                    successor_value =
                                        lower[successor_count]->probe(
                                            successor
                                        );
                                }
                            } else {
                                const std::uint64_t successor_index =
                                    rank_position(successor);

                                std::atomic_ref<std::uint8_t>
                                    successor_atomic(
                                        values[successor_index]
                                    );

                                const std::uint8_t raw =
                                    successor_atomic.load(
                                        std::memory_order_relaxed
                                    );

                                if (raw == UNKNOWN) {
                                    all_successors_win = false;
                                    continue;
                                }

                                successor_value =
                                    static_cast<WLD>(raw);
                            }

                            if (successor_value == WLD::Loss) {
                                has_loss_successor = true;
                                break;
                            }

                            if (successor_value != WLD::Win) {
                                all_successors_win = false;
                            }
                        }

                        std::uint8_t replacement = UNKNOWN;

                        if (has_loss_successor) {
                            replacement =
                                static_cast<std::uint8_t>(WLD::Win);
                        } else if (all_successors_win) {
                            replacement =
                                static_cast<std::uint8_t>(WLD::Loss);
                        }

                        if (replacement != UNKNOWN) {
                            std::uint8_t expected = UNKNOWN;
                            if (current.compare_exchange_strong(
                                    expected,
                                    replacement,
                                    std::memory_order_relaxed)) {
                                changed.fetch_add(
                                    1, std::memory_order_relaxed
                                );
                            }
                        }
                    }
                }
            );

            const auto count =
                changed.load(std::memory_order_relaxed);

            std::cerr << "  iteration " << iteration
                      << ": resolved " << count << '\n';

            if (count == 0) {
                break;
            }
        }

        // Anything outside the win/loss attractor is a draw.
        parallel_ranges(entries, threads,
            [&](std::uint64_t begin, std::uint64_t end) {
                for (std::uint64_t index = begin; index < end; ++index) {
                    if (values[index] == UNKNOWN) {
                        values[index] =
                            static_cast<std::uint8_t>(WLD::Draw);
                    }
                }
            }
        );

        const auto output_file =
            tablebase_name(options.output_prefix, pieces);

        FileHeader header{};
        std::memcpy(header.magic, "WLDDB1", 6);
        header.version = 1;
        header.board_size =
            Rules::for_variant(options.variant).board_size;
        header.pieces = pieces;
        header.playable_squares =
            Rules::for_variant(options.variant).playable_squares;
        header.entries = entries;
        header.data_offset = HEADER_SIZE;
        header.variant = static_cast<std::uint8_t>(options.variant);

        std::vector<std::uint8_t> packed(
            static_cast<std::size_t>((entries + 3) / 4),
            0
        );

        parallel_ranges(packed.size(), threads,
            [&](std::uint64_t begin, std::uint64_t end) {
                for (std::uint64_t byte_index = begin;
                     byte_index < end;
                     ++byte_index) {
                    std::uint8_t byte = 0;

                    for (unsigned slot = 0; slot < 4; ++slot) {
                        const std::uint64_t index =
                            byte_index * 4 + slot;

                        if (index < entries) {
                            byte |= static_cast<std::uint8_t>(
                                (values[index] & 3) << (slot * 2)
                            );
                        }
                    }

                    packed[byte_index] = byte;
                }
            }
        );

        {
            std::ofstream output(output_file, std::ios::binary);
            if (!output) {
                throw std::runtime_error(
                    "cannot create tablebase: " +
                    output_file.string()
                );
            }

            output.write(
                reinterpret_cast<const char*>(&header),
                sizeof(header)
            );
            output.write(
                reinterpret_cast<const char*>(packed.data()),
                static_cast<std::streamsize>(packed.size())
            );

            if (!output) {
                throw std::runtime_error("tablebase write failed");
            }
        }

        write_sha_sidecar(output_file);

        lower[pieces] =
            std::make_unique<Tablebase>(output_file);

        std::cerr << "Wrote " << output_file
                  << "\nSHA-256: "
                  << sha256_file(output_file) << "\n";
    }
}

} // namespace draughts
