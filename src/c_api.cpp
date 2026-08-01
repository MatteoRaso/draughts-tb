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
#include "dwtb/dwtb_c.h"
#include "dwtb/dwtb.hpp"

#include <memory>
#include <string>

namespace {

struct Handle {
    std::unique_ptr<dwtb::Tablebase> tablebase;
    std::string error;
};

} // namespace

extern "C" {

dwtb_handle dwtb_open(
    const char* directory,
    int board_size,
    unsigned cache_mib) {

    if (!directory) return nullptr;

    try {
        auto handle = std::make_unique<Handle>();
        handle->tablebase = std::make_unique<dwtb::Tablebase>(
            directory,
            board_size,
            cache_mib);

        return handle.release();
    } catch (...) {
        return nullptr;
    }
}

int dwtb_probe(
    dwtb_handle opaque,
    uint64_t white,
    uint64_t black,
    uint64_t kings,
    int turn) {

    auto* handle = static_cast<Handle*>(opaque);
    if (!handle || !handle->tablebase) return -1;

    try {
        handle->error.clear();

        const dwtb::Position position{
            white,
            black,
            kings
        };

        const dwtb::Color side =
            turn == 1
                ? dwtb::Color::Black
                : dwtb::Color::White;

        return static_cast<int>(
            handle->tablebase->probe(position, side));
    } catch (const std::exception& e) {
        handle->error = e.what();
        return -1;
    }
}

const char* dwtb_last_error(dwtb_handle opaque) {
    auto* handle = static_cast<Handle*>(opaque);
    if (!handle) return "Invalid dwtb handle";
    return handle->error.c_str();
}

void dwtb_close(dwtb_handle opaque) {
    delete static_cast<Handle*>(opaque);
}

} // extern "C"
