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

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::uint64_t parseInteger(const std::string& text) {
    std::size_t parsed = 0;
    const std::uint64_t value =
        std::stoull(text, &parsed, 0);

    if (parsed != text.size()) {
        throw std::runtime_error(
            "Invalid integer: " + text);
    }
    return value;
}

void usage() {
    std::cerr <<
        "Usage:\n"
        "  dwtb-query --db DIR --board 8|10 "
        "--white MASK --black MASK --kings MASK "
        "--turn white|black\n\n"
        "MASK may be decimal or 0x-prefixed hexadecimal.\n";
}

std::string valueAfter(int& i, int argc, char** argv) {
    if (++i >= argc) {
        throw std::runtime_error("Missing argument value");
    }
    return argv[i];
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::string directory;
        int board = 8;
        dwtb::Position position;
        dwtb::Color turn = dwtb::Color::White;

        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];

            if (argument == "--db") {
                directory = valueAfter(i, argc, argv);
            } else if (argument == "--board") {
                board = std::stoi(valueAfter(i, argc, argv));
            } else if (argument == "--white") {
                position.white =
                    parseInteger(valueAfter(i, argc, argv));
            } else if (argument == "--black") {
                position.black =
                    parseInteger(valueAfter(i, argc, argv));
            } else if (argument == "--kings") {
                position.kings =
                    parseInteger(valueAfter(i, argc, argv));
            } else if (argument == "--turn") {
                const std::string value =
                    valueAfter(i, argc, argv);
                turn = value == "black"
                    ? dwtb::Color::Black
                    : dwtb::Color::White;
            } else if (
                argument == "--help" ||
                argument == "-h") {
                usage();
                return 0;
            } else {
                throw std::runtime_error(
                    "Unknown argument: " + argument);
            }
        }

        if (directory.empty()) {
            usage();
            return 2;
        }

        dwtb::Tablebase tablebase(directory, board);
        const dwtb::Value value =
            tablebase.probe(position, turn);

        std::cout << dwtb::valueName(value) << '\n';
        return value == dwtb::Value::Unknown ? 3 : 0;
    } catch (const std::exception& e) {
        std::cerr << "dwtb-query: " << e.what() << '\n';
        return 1;
    }
}
