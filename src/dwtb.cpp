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
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <list>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <openssl/evp.h>
#include <zstd.h>

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace dwtb {
namespace {

using U128 = unsigned __int128;

constexpr std::array<std::pair<int, int>, 4> Directions{{
    {-1, -1}, {-1, 1}, {1, -1}, {1, 1}
}};

constexpr std::uint8_t FlagCaptureElided = 1;

enum class CellStatus : std::uint32_t {
    Unknown  = 0,
    Win      = 1,
    Loss     = 2,
    Draw     = 3,
    Excluded = 4
};

constexpr std::uint32_t StatusMask = 7;

std::uint32_t packCell(CellStatus status, std::uint32_t remaining) {
    return (remaining << 3) | static_cast<std::uint32_t>(status);
}

CellStatus cellStatus(std::uint32_t cell) {
    return static_cast<CellStatus>(cell & StatusMask);
}

std::uint32_t cellRemaining(std::uint32_t cell) {
    return cell >> 3;
}

Bits bit(int square) {
    return Bits{1} << square;
}

int popcount(Bits b) {
    return std::popcount(b);
}

std::tuple<unsigned, unsigned, unsigned, unsigned> materialTuple(
    const Material& m) {
    return {
        m.whiteMen,
        m.whiteKings,
        m.blackMen,
        m.blackKings
    };
}

std::string hexDigest(const unsigned char* data, unsigned length) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned i = 0; i < length; ++i) {
        out << std::setw(2) << static_cast<unsigned>(data[i]);
    }
    return out.str();
}

std::uint64_t rankerSignature(int board, const Material& m) {
    constexpr std::uint64_t FnvOffset = 1469598103934665603ULL;
    constexpr std::uint64_t FnvPrime  = 1099511628211ULL;

    std::uint64_t h = FnvOffset;
    const std::array<unsigned, 6> values{
        1U,
        static_cast<unsigned>(board),
        m.whiteMen,
        m.whiteKings,
        m.blackMen,
        m.blackKings
    };

    for (unsigned v : values) {
        h ^= v;
        h *= FnvPrime;
    }
    return h;
}

void ensureLittleEndian() {
    if constexpr (std::endian::native != std::endian::little) {
        throw std::runtime_error(
            "The current file format requires a little-endian host");
    }
}

template<class F>
void runThreads(unsigned count, F&& function) {
    if (count == 0) {
        count = 1;
    }

    std::atomic_bool stop{false};
    std::exception_ptr exception;
    std::mutex exceptionMutex;
    std::vector<std::thread> workers;

    workers.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        workers.emplace_back([&, i] {
            try {
                function(i, stop);
            } catch (...) {
                stop.store(true, std::memory_order_relaxed);
                std::lock_guard lock(exceptionMutex);
                if (!exception) {
                    exception = std::current_exception();
                }
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    if (exception) {
        std::rethrow_exception(exception);
    }
}

struct PositionHash {
    std::size_t operator()(const Position& p) const noexcept {
        std::uint64_t h = p.white;
        h ^= std::rotl(p.black, 21);
        h ^= std::rotl(p.kings, 42);
        return static_cast<std::size_t>(h ^ (h >> 32));
    }
};

struct ChildPosition {
    Position p;
    Color side;

    bool operator==(const ChildPosition&) const = default;
};

struct ChildHash {
    std::size_t operator()(const ChildPosition& c) const noexcept {
        PositionHash ph;
        return ph(c.p) ^ (static_cast<std::size_t>(c.side) << 1);
    }
};

Value parentValueFromChildren(
    const std::vector<ChildPosition>& children,
    const Tablebase& tablebase) {

    bool sawDraw = false;
    bool sawUnknown = false;

    for (const auto& child : children) {
        const Value childValue = tablebase.probe(child.p, child.side);

        if (childValue == Value::Loss) {
            return Value::Win;
        }
        if (childValue == Value::Draw) {
            sawDraw = true;
        }
        if (childValue == Value::Unknown) {
            sawUnknown = true;
        }
    }

    if (sawUnknown) return Value::Unknown;
    if (sawDraw)    return Value::Draw;
    return Value::Loss;
}

#pragma pack(push, 1)

struct DiskHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t headerBytes;

    std::uint8_t boardSize;
    std::uint8_t flags;

    std::uint8_t whiteMen;
    std::uint8_t whiteKings;
    std::uint8_t blackMen;
    std::uint8_t blackKings;

    std::uint8_t blockLog2;
    std::uint8_t valueEncoding;
    std::uint16_t reserved;

    std::uint64_t positionCount;
    std::uint64_t blockCount;
    std::uint64_t indexOffset;
    std::uint64_t dataOffset;
    std::uint64_t rankerSignature;
};

struct DiskIndex {
    std::uint64_t offset;
    std::uint32_t storedBytes;
    std::uint32_t rawBytes;
    std::uint8_t codec;      // 0 raw, 1 zstd, 2 constant
    std::uint8_t constant;
    std::uint16_t reserved;
};

#pragma pack(pop)

constexpr char DiskMagic[8]{
    'D', 'W', 'T', 'B', 'W', 'L', 'D', '1'
};

class MappedCells {
public:
    MappedCells(const fs::path& path, std::uint64_t count)
        : path_(path), count_(count), bytes_(count * sizeof(std::uint32_t)) {

        fs::create_directories(path.parent_path());

#ifdef _WIN32
        file_ = CreateFileW(
            path.wstring().c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (file_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("CreateFile failed for " + path.string());
        }

        LARGE_INTEGER size;
        size.QuadPart = static_cast<LONGLONG>(bytes_);

        if (!SetFilePointerEx(file_, size, nullptr, FILE_BEGIN) ||
            !SetEndOfFile(file_)) {
            throw std::runtime_error("Cannot resize " + path.string());
        }

        mapping_ = CreateFileMappingW(
            file_,
            nullptr,
            PAGE_READWRITE,
            size.HighPart,
            size.LowPart,
            nullptr);

        if (!mapping_) {
            throw std::runtime_error(
                "CreateFileMapping failed for " + path.string());
        }

        data_ = static_cast<std::uint32_t*>(
            MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, bytes_));

        if (!data_) {
            throw std::runtime_error(
                "MapViewOfFile failed for " + path.string());
        }
#else
        fd_ = ::open(
            path.c_str(),
            O_RDWR | O_CREAT | O_TRUNC,
            0644);

        if (fd_ < 0) {
            throw std::runtime_error("open failed for " + path.string());
        }

        if (::ftruncate(fd_, static_cast<off_t>(bytes_)) != 0) {
            throw std::runtime_error("ftruncate failed for " + path.string());
        }

        data_ = static_cast<std::uint32_t*>(
            ::mmap(
                nullptr,
                bytes_,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                fd_,
                0));

        if (data_ == MAP_FAILED) {
            data_ = nullptr;
            throw std::runtime_error("mmap failed for " + path.string());
        }
#endif
    }

    ~MappedCells() {
#ifdef _WIN32
        if (data_) {
            FlushViewOfFile(data_, bytes_);
            UnmapViewOfFile(data_);
        }
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
#else
        if (data_) {
            ::msync(data_, bytes_, MS_SYNC);
            ::munmap(data_, bytes_);
        }
        if (fd_ >= 0) ::close(fd_);
#endif
    }

    MappedCells(const MappedCells&) = delete;
    MappedCells& operator=(const MappedCells&) = delete;

    std::uint32_t* data() {
        return data_;
    }

    const std::uint32_t* data() const {
        return data_;
    }

    std::uint64_t size() const {
        return count_;
    }

private:
    fs::path path_;
    std::uint64_t count_;
    std::size_t bytes_;
    std::uint32_t* data_ = nullptr;

#ifdef _WIN32
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
#else
    int fd_ = -1;
#endif
};

void concatenateFiles(
    const std::vector<fs::path>& parts,
    const fs::path& output) {

    fs::remove(output);
    std::ofstream out(output, std::ios::binary);
    if (!out) {
        throw std::runtime_error("Cannot create " + output.string());
    }

    std::vector<char> buffer(1 << 20);

    for (const auto& part : parts) {
        std::ifstream in(part, std::ios::binary);
        if (!in) {
            throw std::runtime_error("Cannot read " + part.string());
        }

        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = in.gcount();
            if (count > 0) {
                out.write(buffer.data(), count);
            }
        }

        in.close();
        fs::remove(part);
    }
}

std::uint64_t frontierCount(const fs::path& path) {
    if (!fs::exists(path)) return 0;
    return fs::file_size(path) / sizeof(std::uint64_t);
}

std::uint8_t valueCode(CellStatus status, std::uint8_t filler) {
    switch (status) {
    case CellStatus::Loss:
        return static_cast<std::uint8_t>(Value::Loss);
    case CellStatus::Win:
        return static_cast<std::uint8_t>(Value::Win);
    case CellStatus::Unknown:
    case CellStatus::Draw:
        return static_cast<std::uint8_t>(Value::Draw);
    case CellStatus::Excluded:
        return filler;
    }
    return static_cast<std::uint8_t>(Value::Draw);
}

struct EncodedBlock {
    std::uint8_t codec = 0;
    std::uint8_t constant = 0;
    std::uint32_t rawBytes = 0;
    std::vector<std::uint8_t> payload;
};

EncodedBlock encodeBlock(
    const std::uint32_t* cells,
    std::uint64_t begin,
    std::uint64_t count,
    int zstdLevel,
    bool tryFillers) {

    const std::uint32_t rawBytes =
        static_cast<std::uint32_t>((count + 3) / 4);

    EncodedBlock best;
    bool haveBest = false;

    const unsigned fillerCount = tryFillers ? 3U : 1U;

    for (unsigned fillerIndex = 0;
         fillerIndex < fillerCount;
         ++fillerIndex) {

        const std::uint8_t filler = tryFillers
            ? static_cast<std::uint8_t>(fillerIndex)
            : static_cast<std::uint8_t>(Value::Draw);

        std::vector<std::uint8_t> raw(rawBytes, 0);

        bool uniform = true;
        std::uint8_t first = 0;

        for (std::uint64_t i = 0; i < count; ++i) {
            const auto status = cellStatus(cells[begin + i]);
            const std::uint8_t code = valueCode(status, filler);

            if (i == 0) first = code;
            else if (code != first) uniform = false;

            raw[i >> 2] |= static_cast<std::uint8_t>(
                code << ((i & 3) * 2));
        }

        EncodedBlock candidate;
        candidate.rawBytes = rawBytes;

        if (uniform) {
            candidate.codec = 2;
            candidate.constant = first;
        } else {
            candidate.codec = 0;
            candidate.payload = raw;

            std::vector<std::uint8_t> compressed(
                ZSTD_compressBound(raw.size()));

            const std::size_t compressedSize = ZSTD_compress(
                compressed.data(),
                compressed.size(),
                raw.data(),
                raw.size(),
                zstdLevel);

            if (ZSTD_isError(compressedSize)) {
                throw std::runtime_error(
                    std::string("ZSTD_compress: ") +
                    ZSTD_getErrorName(compressedSize));
            }

            compressed.resize(compressedSize);

            if (compressed.size() < candidate.payload.size()) {
                candidate.codec = 1;
                candidate.payload = std::move(compressed);
            }
        }

        const std::size_t candidateSize = candidate.payload.size();
        const std::size_t bestSize = best.payload.size();

        if (!haveBest ||
            candidateSize < bestSize ||
            (candidateSize == bestSize &&
             candidate.codec == 2 &&
             best.codec != 2)) {
            best = std::move(candidate);
            haveBest = true;
        }
    }

    return best;
}

} // namespace

bool operator<(const Material& a, const Material& b) {
    return materialTuple(a) < materialTuple(b);
}

Material materialOf(const Position& p) {
    const Bits whiteKings = p.white & p.kings;
    const Bits blackKings = p.black & p.kings;

    return Material{
        static_cast<std::uint8_t>(popcount(p.white & ~p.kings)),
        static_cast<std::uint8_t>(popcount(whiteKings)),
        static_cast<std::uint8_t>(popcount(p.black & ~p.kings)),
        static_cast<std::uint8_t>(popcount(blackKings))
    };
}

Material swappedMaterial(const Material& m) {
    return Material{
        m.blackMen,
        m.blackKings,
        m.whiteMen,
        m.whiteKings
    };
}

bool isCanonicalMaterial(const Material& m) {
    return !(swappedMaterial(m) < m);
}

Position rotate180AndSwapColors(
    const Position& p,
    int squareCount) {

    const auto reverse = [squareCount](Bits source) {
        Bits result = 0;
        while (source) {
            const int from = std::countr_zero(source);
            source &= source - 1;
            result |= bit(squareCount - 1 - from);
        }
        return result;
    };

    return Position{
        reverse(p.black),
        reverse(p.white),
        reverse(p.kings)
    };
}

std::string sliceFilename(int boardSize, const Material& m) {
    std::ostringstream out;
    out << "b" << boardSize
        << "-wm" << static_cast<unsigned>(m.whiteMen)
        << "-wk" << static_cast<unsigned>(m.whiteKings)
        << "-bm" << static_cast<unsigned>(m.blackMen)
        << "-bk" << static_cast<unsigned>(m.blackKings)
        << ".dwtb";
    return out.str();
}

Rules::Rules(int boardSize)
    : boardSize_(boardSize),
      squareCount_(boardSize * boardSize / 2) {

    if (boardSize != 8 && boardSize != 10) {
        throw std::invalid_argument(
            "Only 8x8 and 10x10 boards are supported");
    }
}

int Rules::boardSize() const {
    return boardSize_;
}

int Rules::squareCount() const {
    return squareCount_;
}

int Rules::square(int r, int c) const {
    if (r < 0 || r >= boardSize_ || c < 0 || c >= boardSize_) {
        return -1;
    }
    if (((r + c) & 1) == 0) {
        return -1;
    }
    return r * (boardSize_ / 2) + c / 2;
}

int Rules::row(int sq) const {
    return sq / (boardSize_ / 2);
}

int Rules::column(int sq) const {
    const int r = row(sq);
    const int offset = sq % (boardSize_ / 2);
    return 2 * offset + ((r & 1) ? 0 : 1);
}

bool Rules::isPromotionSquare(Color c, int sq) const {
    const int r = row(sq);
    return c == Color::White ? r == 0 : r == boardSize_ - 1;
}

bool Rules::hasCapture(
    const Position& p,
    Color side) const {

    const Bits own = p.pieces(side);
    const Bits enemy = p.pieces(other(side));
    const Bits occupied = p.occupied();

    Bits pieces = own;

    while (pieces) {
        const int from = std::countr_zero(pieces);
        pieces &= pieces - 1;

        const bool king = (p.kings & bit(from)) != 0;
        const int r = row(from);
        const int c = column(from);

        for (const auto [dr, dc] : Directions) {
            if (!king) {
                const int middle = square(r + dr, c + dc);
                const int landing = square(r + 2 * dr, c + 2 * dc);

                if (middle >= 0 &&
                    landing >= 0 &&
                    (enemy & bit(middle)) &&
                    !(occupied & bit(landing))) {
                    return true;
                }
            } else {
                int rr = r + dr;
                int cc = c + dc;

                while (true) {
                    const int sq = square(rr, cc);
                    if (sq < 0) break;
                    if (occupied & bit(sq)) {
                        if (!(enemy & bit(sq))) break;

                        const int landing = square(rr + dr, cc + dc);
                        if (landing >= 0 &&
                            !(occupied & bit(landing))) {
                            return true;
                        }
                        break;
                    }
                    rr += dr;
                    cc += dc;
                }
            }
        }
    }

    return false;
}

std::vector<Move> Rules::captureMoves(
    const Position& p,
    Color side) const {

    std::vector<Move> result;

    const Bits own = p.pieces(side);
    const Bits enemy = p.pieces(other(side));

    Bits movers = own;

    while (movers) {
        const int from = std::countr_zero(movers);
        movers &= movers - 1;

        const bool movingKing = (p.kings & bit(from)) != 0;

        Position base = p;
        if (side == Color::White) base.white &= ~bit(from);
        else                      base.black &= ~bit(from);
        base.kings &= ~bit(from);

        std::vector<std::uint8_t> landings;
        std::vector<std::uint8_t> captured;

        std::function<void(int, Bits)> recurse =
            [&](int current, Bits capturedMask) {

            const Bits occupied = base.occupied() | bit(current);
            bool found = false;

            const int r = row(current);
            const int c = column(current);

            for (const auto [dr, dc] : Directions) {
                if (!movingKing) {
                    const int middle = square(r + dr, c + dc);
                    const int landing = square(
                        r + 2 * dr,
                        c + 2 * dc);

                    if (middle < 0 || landing < 0) {
                        continue;
                    }

                    const Bits middleBit = bit(middle);
                    const Bits landingBit = bit(landing);

                    if ((enemy & middleBit) &&
                        !(capturedMask & middleBit) &&
                        !(occupied & landingBit)) {

                        found = true;
                        captured.push_back(
                            static_cast<std::uint8_t>(middle));
                        landings.push_back(
                            static_cast<std::uint8_t>(landing));

                        recurse(
                            landing,
                            capturedMask | middleBit);

                        landings.pop_back();
                        captured.pop_back();
                    }
                } else {
                    int rr = r + dr;
                    int cc = c + dc;
                    int hit = -1;

                    while (true) {
                        const int sq = square(rr, cc);
                        if (sq < 0) break;

                        if (occupied & bit(sq)) {
                            hit = sq;
                            break;
                        }

                        rr += dr;
                        cc += dc;
                    }

                    if (hit < 0) continue;

                    const Bits hitBit = bit(hit);

                    if (!(enemy & hitBit) ||
                        (capturedMask & hitBit)) {
                        continue;
                    }

                    rr += dr;
                    cc += dc;

                    while (true) {
                        const int landing = square(rr, cc);
                        if (landing < 0) break;
                        if (occupied & bit(landing)) break;

                        found = true;
                        captured.push_back(
                            static_cast<std::uint8_t>(hit));
                        landings.push_back(
                            static_cast<std::uint8_t>(landing));

                        recurse(
                            landing,
                            capturedMask | hitBit);

                        landings.pop_back();
                        captured.pop_back();

                        rr += dr;
                        cc += dc;
                    }
                }
            }

            if (!found && !captured.empty()) {
                result.push_back(Move{
                    static_cast<std::uint8_t>(from),
                    static_cast<std::uint8_t>(current),
                    landings,
                    captured
                });
            }
        };

        recurse(from, 0);
    }

    std::size_t maximum = 0;
    for (const auto& move : result) {
        maximum = std::max(maximum, move.captured.size());
    }

    result.erase(
        std::remove_if(
            result.begin(),
            result.end(),
            [maximum](const Move& move) {
                return move.captured.size() != maximum;
            }),
        result.end());

    return result;
}

std::vector<Move> Rules::quietMoves(
    const Position& p,
    Color side) const {

    std::vector<Move> result;

    Bits pieces = p.pieces(side);
    const Bits occupied = p.occupied();

    while (pieces) {
        const int from = std::countr_zero(pieces);
        pieces &= pieces - 1;

        const bool king = (p.kings & bit(from)) != 0;
        const int r = row(from);
        const int c = column(from);

        if (!king) {
            const int dr = side == Color::White ? -1 : 1;

            for (int dc : {-1, 1}) {
                const int to = square(r + dr, c + dc);
                if (to >= 0 && !(occupied & bit(to))) {
                    result.push_back(Move{
                        static_cast<std::uint8_t>(from),
                        static_cast<std::uint8_t>(to),
                        {static_cast<std::uint8_t>(to)},
                        {}
                    });
                }
            }
        } else {
            for (const auto [dr, dc] : Directions) {
                int rr = r + dr;
                int cc = c + dc;

                while (true) {
                    const int to = square(rr, cc);
                    if (to < 0) break;
                    if (occupied & bit(to)) break;

                    result.push_back(Move{
                        static_cast<std::uint8_t>(from),
                        static_cast<std::uint8_t>(to),
                        {static_cast<std::uint8_t>(to)},
                        {}
                    });

                    rr += dr;
                    cc += dc;
                }
            }
        }
    }

    return result;
}

std::vector<Move> Rules::legalMoves(
    const Position& p,
    Color side) const {

    auto captures = captureMoves(p, side);
    if (!captures.empty()) {
        return captures;
    }
    return quietMoves(p, side);
}

Position Rules::apply(
    const Position& original,
    Color side,
    const Move& move) const {

    Position result = original;

    const Bits fromBit = bit(move.from);
    const Bits toBit = bit(move.to);
    const bool movingKing = (original.kings & fromBit) != 0;

    if (side == Color::White) {
        result.white &= ~fromBit;
    } else {
        result.black &= ~fromBit;
    }
    result.kings &= ~fromBit;

    for (const int capturedSquare : move.captured) {
        const Bits capturedBit = bit(capturedSquare);
        result.white &= ~capturedBit;
        result.black &= ~capturedBit;
        result.kings &= ~capturedBit;
    }

    if (side == Color::White) {
        result.white |= toBit;
    } else {
        result.black |= toBit;
    }

    if (movingKing || isPromotionSquare(side, move.to)) {
        result.kings |= toBit;
    }

    return result;
}

std::vector<Position> Rules::quietPredecessors(
    const Position& child,
    Color parentSide) const {

    std::vector<Position> result;

    const Bits own = child.pieces(parentSide);
    Bits destinations = own;

    while (destinations) {
        const int destination = std::countr_zero(destinations);
        destinations &= destinations - 1;

        const bool king = (child.kings & bit(destination)) != 0;
        const int r = row(destination);
        const int c = column(destination);

        const auto emit = [&](int origin) {
            Position predecessor = child;

            if (parentSide == Color::White) {
                predecessor.white &= ~bit(destination);
                predecessor.white |= bit(origin);
            } else {
                predecessor.black &= ~bit(destination);
                predecessor.black |= bit(origin);
            }

            predecessor.kings &= ~bit(destination);
            if (king) predecessor.kings |= bit(origin);

            if (!hasCapture(predecessor, parentSide)) {
                result.push_back(predecessor);
            }
        };

        if (!king) {
            const int forwardDr =
                parentSide == Color::White ? -1 : 1;

            for (int forwardDc : {-1, 1}) {
                const int origin = square(
                    r - forwardDr,
                    c - forwardDc);

                if (origin >= 0 &&
                    !(child.occupied() & bit(origin))) {
                    emit(origin);
                }
            }
        } else {
            for (const auto [dr, dc] : Directions) {
                int rr = r + dr;
                int cc = c + dc;

                while (true) {
                    const int origin = square(rr, cc);
                    if (origin < 0) break;
                    if (child.occupied() & bit(origin)) break;

                    emit(origin);

                    rr += dr;
                    cc += dc;
                }
            }
        }
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const Position& a, const Position& b) {
            return std::tie(a.white, a.black, a.kings) <
                   std::tie(b.white, b.black, b.kings);
        });

    result.erase(
        std::unique(result.begin(), result.end()),
        result.end());

    return result;
}

struct Ranker::Impl {
    Rules rules;
    Material material;
    std::unordered_map<std::uint64_t, U128> counts;
    std::uint64_t total = 0;

    Impl(int board, Material m)
        : rules(board), material(m) {

        if (m.total() == 0) {
            throw std::invalid_argument(
                "Ranker requires at least one piece");
        }

        const U128 value = build(
            0,
            m.whiteMen,
            m.whiteKings,
            m.blackMen,
            m.blackKings);

        if (value > std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error(
                "Slice has more than 2^64-1 positions");
        }

        total = static_cast<std::uint64_t>(value);
    }

    std::uint64_t key(
        int position,
        int wm,
        int wk,
        int bm,
        int bk) const {

        return static_cast<std::uint64_t>(position)
            | (static_cast<std::uint64_t>(wm) << 6)
            | (static_cast<std::uint64_t>(wk) << 12)
            | (static_cast<std::uint64_t>(bm) << 18)
            | (static_cast<std::uint64_t>(bk) << 24);
    }

    bool allowed(int type, int square) const {
        const int r = rules.row(square);

        if (type == 1) return r != 0;
        if (type == 3) return r != rules.boardSize() - 1;
        return true;
    }

    U128 build(
        int position,
        int wm,
        int wk,
        int bm,
        int bk) {

        if (wm < 0 || wk < 0 || bm < 0 || bk < 0) {
            return 0;
        }

        if (wm + wk + bm + bk >
            rules.squareCount() - position) {
            return 0;
        }

        const auto k = key(position, wm, wk, bm, bk);
        if (auto it = counts.find(k); it != counts.end()) {
            return it->second;
        }

        U128 value = 0;

        if (position == rules.squareCount()) {
            value = (wm == 0 && wk == 0 && bm == 0 && bk == 0)
                ? 1
                : 0;
        } else {
            value += build(position + 1, wm, wk, bm, bk);

            if (wm && allowed(1, position))
                value += build(position + 1, wm - 1, wk, bm, bk);
            if (wk)
                value += build(position + 1, wm, wk - 1, bm, bk);
            if (bm && allowed(3, position))
                value += build(position + 1, wm, wk, bm - 1, bk);
            if (bk)
                value += build(position + 1, wm, wk, bm, bk - 1);
        }

        counts.emplace(k, value);
        return value;
    }

    U128 lookup(
        int position,
        int wm,
        int wk,
        int bm,
        int bk) const {

        if (wm < 0 || wk < 0 || bm < 0 || bk < 0) {
            return 0;
        }

        const auto it = counts.find(key(position, wm, wk, bm, bk));
        return it == counts.end() ? 0 : it->second;
    }

    int pieceType(const Position& p, int square) const {
        const Bits b = bit(square);
        if (!(p.occupied() & b)) return 0;

        if (p.white & b) {
            return (p.kings & b) ? 2 : 1;
        }
        return (p.kings & b) ? 4 : 3;
    }

    void decrement(
        int type,
        int& wm,
        int& wk,
        int& bm,
        int& bk) const {

        if (type == 1) --wm;
        if (type == 2) --wk;
        if (type == 3) --bm;
        if (type == 4) --bk;
    }
};

Ranker::Ranker(int boardSize, Material material)
    : impl_(std::make_shared<Impl>(boardSize, material)) {}

std::uint64_t Ranker::positions() const {
    return impl_->total;
}

const Material& Ranker::material() const {
    return impl_->material;
}

bool Ranker::valid(const Position& p) const {
    const Bits validMask =
        (Bits{1} << impl_->rules.squareCount()) - 1;

    if ((p.white | p.black | p.kings) & ~validMask) return false;
    if (p.white & p.black) return false;
    if (p.kings & ~p.occupied()) return false;
    if (!(materialOf(p) == impl_->material)) return false;

    for (int sq = 0; sq < impl_->rules.squareCount(); ++sq) {
        const Bits b = bit(sq);
        const int r = impl_->rules.row(sq);

        if ((p.white & b) &&
            !(p.kings & b) &&
            r == 0) {
            return false;
        }

        if ((p.black & b) &&
            !(p.kings & b) &&
            r == impl_->rules.boardSize() - 1) {
            return false;
        }
    }

    return true;
}

std::uint64_t Ranker::rank(const Position& p) const {
    if (!valid(p)) {
        throw std::invalid_argument(
            "Position is not valid for this material slice");
    }

    int wm = impl_->material.whiteMen;
    int wk = impl_->material.whiteKings;
    int bm = impl_->material.blackMen;
    int bk = impl_->material.blackKings;

    U128 rankValue = 0;

    for (int position = 0;
         position < impl_->rules.squareCount();
         ++position) {

        const int actual = impl_->pieceType(p, position);

        for (int type = 0; type < actual; ++type) {
            if (type != 0 && !impl_->allowed(type, position)) {
                continue;
            }

            int twm = wm;
            int twk = wk;
            int tbm = bm;
            int tbk = bk;

            impl_->decrement(type, twm, twk, tbm, tbk);

            rankValue += impl_->lookup(
                position + 1,
                twm,
                twk,
                tbm,
                tbk);
        }

        impl_->decrement(actual, wm, wk, bm, bk);
    }

    return static_cast<std::uint64_t>(rankValue);
}

Position Ranker::unrank(std::uint64_t rankValue) const {
    if (rankValue >= impl_->total) {
        throw std::out_of_range("Rank is outside slice");
    }

    Position result;

    int wm = impl_->material.whiteMen;
    int wk = impl_->material.whiteKings;
    int bm = impl_->material.blackMen;
    int bk = impl_->material.blackKings;

    U128 remaining = rankValue;

    for (int position = 0;
         position < impl_->rules.squareCount();
         ++position) {

        bool selected = false;

        for (int type = 0; type <= 4; ++type) {
            if (type != 0 && !impl_->allowed(type, position)) {
                continue;
            }

            int twm = wm;
            int twk = wk;
            int tbm = bm;
            int tbk = bk;
            impl_->decrement(type, twm, twk, tbm, tbk);

            const U128 count = impl_->lookup(
                position + 1,
                twm,
                twk,
                tbm,
                tbk);

            if (remaining >= count) {
                remaining -= count;
                continue;
            }

            const Bits b = bit(position);

            if (type == 1 || type == 2) result.white |= b;
            if (type == 3 || type == 4) result.black |= b;
            if (type == 2 || type == 4) result.kings |= b;

            wm = twm;
            wk = twk;
            bm = tbm;
            bk = tbk;
            selected = true;
            break;
        }

        if (!selected) {
            throw std::logic_error("Internal unranking failure");
        }
    }

    return result;
}

namespace {

class Slice;

struct CacheKey {
    const Slice* slice = nullptr;
    std::uint64_t block = 0;

    bool operator==(const CacheKey&) const = default;
};

struct CacheKeyHash {
    std::size_t operator()(const CacheKey& key) const noexcept {
        const auto p = reinterpret_cast<std::uintptr_t>(key.slice);
        return static_cast<std::size_t>(
            p ^ (key.block * 0x9e3779b97f4a7c15ULL));
    }
};

class BlockCache {
public:
    explicit BlockCache(std::size_t bytes)
        : budget_(std::max<std::size_t>(bytes, 1 << 20)) {}

    std::shared_ptr<std::vector<std::uint8_t>> get(
        const std::shared_ptr<Slice>& slice,
        std::uint64_t block);

private:
    struct Entry {
        std::shared_ptr<std::vector<std::uint8_t>> data;
        std::list<CacheKey>::iterator lru;
    };

    std::size_t budget_;
    std::size_t used_ = 0;
    std::mutex mutex_;
    std::list<CacheKey> lru_;
    std::unordered_map<CacheKey, Entry, CacheKeyHash> entries_;
};

class Slice : public std::enable_shared_from_this<Slice> {
public:
    explicit Slice(fs::path path)
        : path_(std::move(path)),
          stream_(path_, std::ios::binary) {

        if (!stream_) {
            throw std::runtime_error(
                "Cannot open tablebase " + path_.string());
        }

        readAt(0, &header_, sizeof(header_));

        if (std::memcmp(header_.magic, DiskMagic, sizeof(DiskMagic)) != 0 ||
            header_.version != 1 ||
            header_.headerBytes != sizeof(DiskHeader)) {
            throw std::runtime_error(
                "Invalid tablebase header in " + path_.string());
        }

        material_ = Material{
            header_.whiteMen,
            header_.whiteKings,
            header_.blackMen,
            header_.blackKings
        };

        ranker_ = std::make_unique<Ranker>(
            header_.boardSize,
            material_);

        if (header_.rankerSignature !=
            rankerSignature(header_.boardSize, material_)) {
            throw std::runtime_error(
                "Ranker signature mismatch in " + path_.string());
        }

        index_.resize(header_.blockCount);

        if (!index_.empty()) {
            readAt(
                header_.indexOffset,
                index_.data(),
                index_.size() * sizeof(DiskIndex));
        }
    }

    int boardSize() const {
        return header_.boardSize;
    }

    const Material& material() const {
        return material_;
    }

    std::uint64_t positionCount() const {
        return header_.positionCount;
    }

    std::uint8_t blockLog2() const {
        return header_.blockLog2;
    }

    const Ranker& ranker() const {
        return *ranker_;
    }

    std::shared_ptr<std::vector<std::uint8_t>> loadBlock(
        std::uint64_t block) const {

        if (block >= index_.size()) {
            throw std::out_of_range("Block index outside tablebase");
        }

        const DiskIndex& entry = index_[block];

        auto raw = std::make_shared<std::vector<std::uint8_t>>(
            entry.rawBytes);

        if (entry.codec == 2) {
            const std::uint8_t repeated =
                static_cast<std::uint8_t>(
                    entry.constant |
                    (entry.constant << 2) |
                    (entry.constant << 4) |
                    (entry.constant << 6));

            std::fill(raw->begin(), raw->end(), repeated);
            return raw;
        }

        std::vector<std::uint8_t> stored(entry.storedBytes);
        if (!stored.empty()) {
            readAt(entry.offset, stored.data(), stored.size());
        }

        if (entry.codec == 0) {
            if (stored.size() != raw->size()) {
                throw std::runtime_error(
                    "Raw block size mismatch in " + path_.string());
            }
            *raw = std::move(stored);
            return raw;
        }

        if (entry.codec == 1) {
            const std::size_t result = ZSTD_decompress(
                raw->data(),
                raw->size(),
                stored.data(),
                stored.size());

            if (ZSTD_isError(result) || result != raw->size()) {
                throw std::runtime_error(
                    "Zstandard decode failure in " + path_.string());
            }

            return raw;
        }

        throw std::runtime_error(
            "Unknown block codec in " + path_.string());
    }

private:
    void readAt(
        std::uint64_t offset,
        void* destination,
        std::size_t size) const {

        std::lock_guard lock(ioMutex_);

        stream_.clear();
        stream_.seekg(
            static_cast<std::streamoff>(offset),
            std::ios::beg);

        stream_.read(
            static_cast<char*>(destination),
            static_cast<std::streamsize>(size));

        if (stream_.gcount() != static_cast<std::streamsize>(size)) {
            throw std::runtime_error(
                "Short read from " + path_.string());
        }
    }

    fs::path path_;
    mutable std::ifstream stream_;
    mutable std::mutex ioMutex_;

    DiskHeader header_{};
    Material material_{};
    std::unique_ptr<Ranker> ranker_;
    std::vector<DiskIndex> index_;
};

std::shared_ptr<std::vector<std::uint8_t>> BlockCache::get(
    const std::shared_ptr<Slice>& slice,
    std::uint64_t block) {

    const CacheKey key{slice.get(), block};

    {
        std::lock_guard lock(mutex_);

        if (auto it = entries_.find(key); it != entries_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second.lru);
            return it->second.data;
        }
    }

    auto loaded = slice->loadBlock(block);

    std::lock_guard lock(mutex_);

    if (auto it = entries_.find(key); it != entries_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        return it->second.data;
    }

    lru_.push_front(key);
    used_ += loaded->size();

    entries_.emplace(
        key,
        Entry{loaded, lru_.begin()});

    while (used_ > budget_ && entries_.size() > 1) {
        const CacheKey victim = lru_.back();
        auto it = entries_.find(victim);

        used_ -= it->second.data->size();
        lru_.pop_back();
        entries_.erase(it);
    }

    return loaded;
}

} // namespace

struct Tablebase::Impl {
    fs::path directory;
    Rules rules;
    mutable BlockCache cache;

    mutable std::mutex sliceMutex;
    mutable std::unordered_map<
        std::string,
        std::shared_ptr<Slice>> slices;

    Impl(fs::path dir, int board, std::size_t cacheMiB)
        : directory(std::move(dir)),
          rules(board),
          cache(cacheMiB * 1024ULL * 1024ULL) {}

    std::shared_ptr<Slice> getSlice(const Material& material) const {
        const std::string name =
            sliceFilename(rules.boardSize(), material);

        {
            std::lock_guard lock(sliceMutex);
            if (auto it = slices.find(name); it != slices.end()) {
                return it->second;
            }
        }

        const fs::path path = directory / name;
        if (!fs::exists(path)) {
            return {};
        }

        auto slice = std::make_shared<Slice>(path);

        std::lock_guard lock(sliceMutex);
        auto [it, inserted] = slices.emplace(name, slice);
        return inserted ? slice : it->second;
    }

    Value probe(Position p, Color side) const {
        const Bits own = p.pieces(side);
        const Bits opponent = p.pieces(other(side));

        if (own == 0) return Value::Loss;
        if (opponent == 0) return Value::Win;

        if (rules.hasCapture(p, side)) {
            const auto moves = rules.captureMoves(p, side);

            bool sawDraw = false;
            bool sawUnknown = false;

            for (const auto& move : moves) {
                const Position child = rules.apply(p, side, move);
                const Value value = probe(child, other(side));

                if (value == Value::Loss) return Value::Win;
                if (value == Value::Draw) sawDraw = true;
                if (value == Value::Unknown) sawUnknown = true;
            }

            if (sawUnknown) return Value::Unknown;
            if (sawDraw) return Value::Draw;
            return Value::Loss;
        }

        Material material = materialOf(p);

        if (!isCanonicalMaterial(material)) {
            p = rotate180AndSwapColors(
                p,
                rules.squareCount());
            side = other(side);
            material = materialOf(p);
        }

        const auto slice = getSlice(material);
        if (!slice) {
            return Value::Unknown;
        }

        if (!slice->ranker().valid(p)) {
            return Value::Unknown;
        }

        const std::uint64_t placement = slice->ranker().rank(p);
        const std::uint64_t id =
            placement * 2 + static_cast<std::uint8_t>(side);

        if (id >= slice->positionCount()) {
            return Value::Unknown;
        }

        const std::uint64_t blockSize =
            std::uint64_t{1} << slice->blockLog2();

        const std::uint64_t block = id / blockSize;
        const std::uint64_t local = id % blockSize;

        const auto raw = cache.get(slice, block);

        const std::uint8_t code = static_cast<std::uint8_t>(
            ((*raw)[local >> 2] >> ((local & 3) * 2)) & 3);

        return static_cast<Value>(code);
    }
};

Tablebase::Tablebase(
    fs::path directory,
    int boardSize,
    std::size_t cacheMiB)
    : impl_(std::make_shared<Impl>(
          std::move(directory),
          boardSize,
          cacheMiB)) {

    ensureLittleEndian();
}

Value Tablebase::probe(
    const Position& position,
    Color sideToMove) const {

    return impl_->probe(position, sideToMove);
}

int Tablebase::boardSize() const {
    return impl_->rules.boardSize();
}

std::string valueName(Value value) {
    switch (value) {
    case Value::Loss:    return "loss";
    case Value::Draw:    return "draw";
    case Value::Win:     return "win";
    case Value::Unknown: return "unknown";
    }
    return "unknown";
}

std::string sha256File(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "Cannot hash " + path.string());
    }

    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context) {
        throw std::runtime_error("EVP_MD_CTX_new failed");
    }

    const auto cleanup = [&] {
        EVP_MD_CTX_free(context);
    };

    if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
        cleanup();
        throw std::runtime_error("EVP_DigestInit_ex failed");
    }

    std::vector<char> buffer(1 << 20);

    while (input) {
        input.read(buffer.data(), buffer.size());
        const auto count = input.gcount();

        if (count > 0 &&
            EVP_DigestUpdate(
                context,
                buffer.data(),
                static_cast<std::size_t>(count)) != 1) {
            cleanup();
            throw std::runtime_error("EVP_DigestUpdate failed");
        }
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned length = 0;

    if (EVP_DigestFinal_ex(context, digest, &length) != 1) {
        cleanup();
        throw std::runtime_error("EVP_DigestFinal_ex failed");
    }

    cleanup();
    return hexDigest(digest, length);
}

namespace {

void writeShaSidecar(const fs::path& path) {
    const std::string digest = sha256File(path);
    std::ofstream out(path.string() + ".sha256");
    out << digest << "  " << path.filename().string() << '\n';
}

std::vector<Material> materialsFor(unsigned totalPieces) {
    std::vector<Material> result;

    for (unsigned wm = 0; wm <= totalPieces; ++wm) {
        for (unsigned wk = 0; wk <= totalPieces - wm; ++wk) {
            for (unsigned bm = 0;
                 bm <= totalPieces - wm - wk;
                 ++bm) {

                const unsigned bk =
                    totalPieces - wm - wk - bm;

                if (wm + wk == 0 || bm + bk == 0) {
                    continue;
                }

                Material m{
                    static_cast<std::uint8_t>(wm),
                    static_cast<std::uint8_t>(wk),
                    static_cast<std::uint8_t>(bm),
                    static_cast<std::uint8_t>(bk)
                };

                if (isCanonicalMaterial(m)) {
                    result.push_back(m);
                }
            }
        }
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const Material& a, const Material& b) {
            if (a.totalKings() != b.totalKings()) {
                return a.totalKings() > b.totalKings();
            }
            return a < b;
        });

    return result;
}

std::vector<ChildPosition> uniqueChildren(
    const Rules& rules,
    const Position& p,
    Color side,
    const std::vector<Move>& moves) {

    std::unordered_set<ChildPosition, ChildHash> unique;

    for (const auto& move : moves) {
        unique.insert(ChildPosition{
            rules.apply(p, side, move),
            other(side)
        });
    }

    return {unique.begin(), unique.end()};
}

void writeCompressedSlice(
    const fs::path& outputPath,
    const std::uint32_t* cells,
    std::uint64_t stateCount,
    int boardSize,
    const Material& material,
    unsigned blockLog2,
    int zstdLevel,
    bool captureElided,
    unsigned threads) {

    const std::uint64_t blockSize =
        std::uint64_t{1} << blockLog2;

    const std::uint64_t blockCount =
        (stateCount + blockSize - 1) / blockSize;

    DiskHeader header{};
    std::memcpy(header.magic, DiskMagic, sizeof(DiskMagic));
    header.version = 1;
    header.headerBytes = sizeof(DiskHeader);
    header.boardSize = static_cast<std::uint8_t>(boardSize);
    header.flags = captureElided ? FlagCaptureElided : 0;
    header.whiteMen = material.whiteMen;
    header.whiteKings = material.whiteKings;
    header.blackMen = material.blackMen;
    header.blackKings = material.blackKings;
    header.blockLog2 = static_cast<std::uint8_t>(blockLog2);
    header.valueEncoding = 1;
    header.positionCount = stateCount;
    header.blockCount = blockCount;
    header.indexOffset = sizeof(DiskHeader);
    header.dataOffset =
        sizeof(DiskHeader) +
        blockCount * sizeof(DiskIndex);
    header.rankerSignature =
        rankerSignature(boardSize, material);

    std::vector<DiskIndex> index(blockCount);

    std::ofstream output(
        outputPath,
        std::ios::binary | std::ios::trunc);

    if (!output) {
        throw std::runtime_error(
            "Cannot create " + outputPath.string());
    }

    output.seekp(
        static_cast<std::streamoff>(header.dataOffset),
        std::ios::beg);

    const std::uint64_t batchSize =
        std::max<std::uint64_t>(threads * 4ULL, 1ULL);

    for (std::uint64_t batchBegin = 0;
         batchBegin < blockCount;
         batchBegin += batchSize) {

        const std::uint64_t count =
            std::min(batchSize, blockCount - batchBegin);

        std::vector<EncodedBlock> encoded(count);
        std::atomic<std::uint64_t> next{0};

        runThreads(threads, [&](unsigned, std::atomic_bool& stop) {
            while (!stop.load(std::memory_order_relaxed)) {
                const std::uint64_t local =
                    next.fetch_add(1, std::memory_order_relaxed);

                if (local >= count) break;

                const std::uint64_t block =
                    batchBegin + local;
                const std::uint64_t begin =
                    block * blockSize;
                const std::uint64_t logicalCount =
                    std::min(blockSize, stateCount - begin);

                encoded[local] = encodeBlock(
                    cells,
                    begin,
                    logicalCount,
                    zstdLevel,
                    captureElided);
            }
        });

        for (std::uint64_t local = 0; local < count; ++local) {
            const std::uint64_t block = batchBegin + local;
            auto& item = encoded[local];

            DiskIndex entry{};
            entry.offset = static_cast<std::uint64_t>(
                output.tellp());
            entry.storedBytes =
                static_cast<std::uint32_t>(item.payload.size());
            entry.rawBytes = item.rawBytes;
            entry.codec = item.codec;
            entry.constant = item.constant;

            if (!item.payload.empty()) {
                output.write(
                    reinterpret_cast<const char*>(
                        item.payload.data()),
                    static_cast<std::streamsize>(
                        item.payload.size()));
            }

            index[block] = entry;
        }
    }

    output.seekp(0, std::ios::beg);
    output.write(
        reinterpret_cast<const char*>(&header),
        sizeof(header));

    if (!index.empty()) {
        output.write(
            reinterpret_cast<const char*>(index.data()),
            static_cast<std::streamsize>(
                index.size() * sizeof(DiskIndex)));
    }

    output.close();
    writeShaSidecar(outputPath);
}

void generateSlice(
    const GenerateOptions& options,
    const Material& material,
    Tablebase& existing) {

    const Rules rules(options.boardSize);
    const Ranker ranker(options.boardSize, material);

    const std::uint64_t placements = ranker.positions();

    if (placements >
        std::numeric_limits<std::uint64_t>::max() / 2) {
        throw std::overflow_error("Side-to-move index overflow");
    }

    const std::uint64_t stateCount = placements * 2;
    const std::string filename =
        sliceFilename(options.boardSize, material);

    const fs::path outputPath =
        options.outputDirectory / filename;
    const fs::path statePath =
        options.temporaryDirectory / (filename + ".state");
    const fs::path frontierA =
        options.temporaryDirectory / (filename + ".frontier-a");
    const fs::path frontierB =
        options.temporaryDirectory / (filename + ".frontier-b");

    if (fs::exists(outputPath) && !options.force) {
        std::cerr << "Skipping existing " << filename << '\n';
        return;
    }

    fs::create_directories(options.outputDirectory);
    fs::create_directories(options.temporaryDirectory);

    std::cerr
        << "Generating " << filename
        << ": " << placements << " placements, "
        << stateCount << " side-to-move states\n";

    {
        MappedCells mapped(statePath, stateCount);
        std::uint32_t* cells = mapped.data();

        const unsigned threads = std::max(1U, options.threads);
        const std::uint64_t placementChunk = 4096;
        std::atomic<std::uint64_t> nextPlacement{0};

        std::vector<fs::path> initialParts(threads);

        runThreads(threads, [&](unsigned tid, std::atomic_bool& stop) {
            const fs::path part =
                options.temporaryDirectory /
                (filename + ".init." + std::to_string(tid));

            initialParts[tid] = part;
            std::ofstream frontier(part, std::ios::binary);

            while (!stop.load(std::memory_order_relaxed)) {
                const std::uint64_t begin =
                    nextPlacement.fetch_add(
                        placementChunk,
                        std::memory_order_relaxed);

                if (begin >= placements) break;

                const std::uint64_t end =
                    std::min(
                        placements,
                        begin + placementChunk);

                for (std::uint64_t placement = begin;
                     placement < end;
                     ++placement) {

                    const Position p = ranker.unrank(placement);

                    for (unsigned sideIndex = 0;
                         sideIndex < 2;
                         ++sideIndex) {

                        const Color side =
                            static_cast<Color>(sideIndex);
                        const std::uint64_t id =
                            placement * 2 + sideIndex;

                        CellStatus status = CellStatus::Unknown;
                        std::uint32_t remaining = 0;

                        if (rules.hasCapture(p, side)) {
                            if (options.elideCaptures) {
                                status = CellStatus::Excluded;
                            } else {
                                const auto children = uniqueChildren(
                                    rules,
                                    p,
                                    side,
                                    rules.captureMoves(p, side));

                                const Value value =
                                    parentValueFromChildren(
                                        children,
                                        existing);

                                if (value == Value::Unknown) {
                                    throw std::runtime_error(
                                        "Missing lower-piece dependency");
                                }

                                status =
                                    value == Value::Win
                                        ? CellStatus::Win
                                    : value == Value::Loss
                                        ? CellStatus::Loss
                                        : CellStatus::Draw;
                            }
                        } else {
                            const auto quiets =
                                rules.quietMoves(p, side);

                            if (quiets.empty()) {
                                status = CellStatus::Loss;
                            } else {
                                const auto children =
                                    uniqueChildren(
                                        rules,
                                        p,
                                        side,
                                        quiets);

                                bool winningMove = false;

                                for (const auto& child : children) {
                                    const Material childMaterial =
                                        materialOf(child.p);

                                    const bool internal =
                                        childMaterial == material &&
                                        !rules.hasCapture(
                                            child.p,
                                            child.side);

                                    if (internal) {
                                        ++remaining;
                                        continue;
                                    }

                                    const Value value = existing.probe(
                                        child.p,
                                        child.side);

                                    if (value == Value::Unknown) {
                                        throw std::runtime_error(
                                            "Missing promotion or "
                                            "capture dependency");
                                    }

                                    if (value == Value::Loss) {
                                        winningMove = true;
                                        break;
                                    }

                                    if (value == Value::Draw) {
                                        // Permanent unresolved edge.
                                        ++remaining;
                                    }
                                }

                                if (winningMove) {
                                    status = CellStatus::Win;
                                } else if (remaining == 0) {
                                    status = CellStatus::Loss;
                                }
                            }
                        }

                        cells[id] = packCell(status, remaining);

                        if (status == CellStatus::Win ||
                            status == CellStatus::Loss) {
                            frontier.write(
                                reinterpret_cast<const char*>(&id),
                                sizeof(id));
                        }
                    }
                }
            }
        });

        concatenateFiles(initialParts, frontierA);

        std::uint64_t pass = 0;

        while (frontierCount(frontierA) != 0) {
            const std::uint64_t records =
                frontierCount(frontierA);

            std::cerr
                << "  retrograde pass " << pass
                << ": " << records << " resolved states\n";

            std::atomic<std::uint64_t> nextRecord{0};
            constexpr std::uint64_t RecordChunk = 32768;

            std::vector<fs::path> parts(threads);

            runThreads(threads, [&](unsigned tid, std::atomic_bool& stop) {
                std::ifstream input(frontierA, std::ios::binary);

                const fs::path part =
                    options.temporaryDirectory /
                    (filename + ".pass-" +
                     std::to_string(pass) + "." +
                     std::to_string(tid));

                parts[tid] = part;
                std::ofstream output(part, std::ios::binary);
                std::vector<std::uint64_t> buffer(RecordChunk);

                while (!stop.load(std::memory_order_relaxed)) {
                    const std::uint64_t begin =
                        nextRecord.fetch_add(
                            RecordChunk,
                            std::memory_order_relaxed);

                    if (begin >= records) break;

                    const std::uint64_t count =
                        std::min(
                            RecordChunk,
                            records - begin);

                    input.clear();
                    input.seekg(
                        static_cast<std::streamoff>(
                            begin * sizeof(std::uint64_t)),
                        std::ios::beg);

                    input.read(
                        reinterpret_cast<char*>(buffer.data()),
                        static_cast<std::streamsize>(
                            count * sizeof(std::uint64_t)));

                    if (input.gcount() !=
                        static_cast<std::streamsize>(
                            count * sizeof(std::uint64_t))) {
                        throw std::runtime_error(
                            "Short frontier read");
                    }

                    for (std::uint64_t i = 0; i < count; ++i) {
                        const std::uint64_t childId = buffer[i];
                        const std::uint32_t childCell =
                            std::atomic_ref<std::uint32_t>(
                                cells[childId]).load(
                                    std::memory_order_relaxed);

                        const CellStatus childStatus =
                            cellStatus(childCell);

                        if (childStatus != CellStatus::Win &&
                            childStatus != CellStatus::Loss) {
                            continue;
                        }

                        const Position child =
                            ranker.unrank(childId / 2);
                        const Color childSide =
                            static_cast<Color>(childId & 1);
                        const Color parentSide =
                            other(childSide);

                        const auto predecessors =
                            rules.quietPredecessors(
                                child,
                                parentSide);

                        for (const Position& predecessor :
                             predecessors) {

                            if (!ranker.valid(predecessor)) {
                                continue;
                            }

                            const std::uint64_t predecessorId =
                                ranker.rank(predecessor) * 2 +
                                static_cast<std::uint8_t>(
                                    parentSide);

                            std::atomic_ref<std::uint32_t> target(
                                cells[predecessorId]);

                            std::uint32_t old =
                                target.load(
                                    std::memory_order_relaxed);

                            while (cellStatus(old) ==
                                   CellStatus::Unknown) {

                                std::uint32_t replacement = old;

                                if (childStatus ==
                                    CellStatus::Loss) {
                                    replacement = packCell(
                                        CellStatus::Win,
                                        0);
                                } else {
                                    const std::uint32_t remaining =
                                        cellRemaining(old);

                                    if (remaining == 0) {
                                        break;
                                    }

                                    replacement =
                                        remaining == 1
                                            ? packCell(
                                                CellStatus::Loss,
                                                0)
                                            : packCell(
                                                CellStatus::Unknown,
                                                remaining - 1);
                                }

                                if (target.compare_exchange_weak(
                                        old,
                                        replacement,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {

                                    const CellStatus newStatus =
                                        cellStatus(replacement);

                                    if (newStatus == CellStatus::Win ||
                                        newStatus == CellStatus::Loss) {
                                        output.write(
                                            reinterpret_cast<const char*>(
                                                &predecessorId),
                                            sizeof(predecessorId));
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            });

            concatenateFiles(parts, frontierB);
            fs::remove(frontierA);
            fs::rename(frontierB, frontierA);
            ++pass;
        }

        fs::remove(frontierA);

        writeCompressedSlice(
            outputPath,
            cells,
            stateCount,
            options.boardSize,
            material,
            options.blockLog2,
            options.zstdLevel,
            options.elideCaptures,
            threads);
    }

    if (!options.keepTemporaryFiles) {
        fs::remove(statePath);
        fs::remove(frontierA);
        fs::remove(frontierB);
    }
}

void writeManifest(
    const GenerateOptions& options) {

    std::vector<fs::path> files;

    for (const auto& entry :
         fs::directory_iterator(options.outputDirectory)) {
        if (entry.path().extension() == ".dwtb") {
            files.push_back(entry.path());
        }
    }

    std::sort(files.begin(), files.end());

    const fs::path manifest =
        options.outputDirectory / "manifest.json";

    std::ofstream out(manifest);

    out << "{\n"
        << "  \"format\": 1,\n"
        << "  \"board_size\": " << options.boardSize << ",\n"
        << "  \"max_pieces\": " << options.maxPieces << ",\n"
        << "  \"capture_elision\": "
        << (options.elideCaptures ? "true" : "false")
        << ",\n"
        << "  \"files\": [\n";

    for (std::size_t i = 0; i < files.size(); ++i) {
        out << "    {\"name\": \""
            << files[i].filename().string()
            << "\", \"bytes\": "
            << fs::file_size(files[i])
            << ", \"sha256\": \""
            << sha256File(files[i])
            << "\"}";

        if (i + 1 != files.size()) out << ',';
        out << '\n';
    }

    out << "  ]\n}\n";
    out.close();

    writeShaSidecar(manifest);
}

} // namespace

void generateTablebases(const GenerateOptions& supplied) {
    ensureLittleEndian();

    GenerateOptions options = supplied;

    if (options.maxPieces < 2) {
        throw std::invalid_argument(
            "maxPieces must be at least 2");
    }

    if (options.threads == 0) {
        options.threads = std::max(
            1U,
            std::thread::hardware_concurrency());
    }

    if (options.blockLog2 < 10 ||
        options.blockLog2 > 26) {
        throw std::invalid_argument(
            "blockLog2 must be in [10, 26]");
    }

    if (options.outputDirectory.empty()) {
        throw std::invalid_argument(
            "outputDirectory is required");
    }

    if (options.temporaryDirectory.empty()) {
        options.temporaryDirectory =
            options.outputDirectory / ".tmp";
    }

    fs::create_directories(options.outputDirectory);
    fs::create_directories(options.temporaryDirectory);

    Tablebase existing(
        options.outputDirectory,
        options.boardSize,
        512);

    for (int pieces = 2;
         pieces <= options.maxPieces;
         ++pieces) {

        for (const Material& material :
             materialsFor(pieces)) {
            generateSlice(options, material, existing);
        }
    }

    writeManifest(options);
}

} // namespace dwtb
