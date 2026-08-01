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

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void usage() {
    std::cerr <<
        "Usage:\n"
        "  dwtb-generate --out DIR --pieces N [options]\n\n"
        "Options:\n"
        "  --board brazilian|international|8|10\n"
        "  --threads N\n"
        "  --temp DIR\n"
        "  --block-log2 N\n"
        "  --zstd-level N\n"
        "  --store-captures\n"
        "  --keep-temp\n"
        "  --force\n";
}

std::string requireValue(int& i, int argc, char** argv) {
    if (++i >= argc) {
        throw std::runtime_error(
            "Missing value after " + std::string(argv[i - 1]));
    }
    return argv[i];
}

} // namespace

int main(int argc, char** argv) {
    try {
        dwtb::GenerateOptions options;

        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];

            if (argument == "--out") {
                options.outputDirectory =
                    requireValue(i, argc, argv);
            } else if (argument == "--temp") {
                options.temporaryDirectory =
                    requireValue(i, argc, argv);
            } else if (argument == "--pieces") {
                options.maxPieces =
                    std::stoi(requireValue(i, argc, argv));
            } else if (argument == "--threads") {
                options.threads = static_cast<unsigned>(
                    std::stoul(requireValue(i, argc, argv)));
            } else if (argument == "--block-log2") {
                options.blockLog2 = static_cast<unsigned>(
                    std::stoul(requireValue(i, argc, argv)));
            } else if (argument == "--zstd-level") {
                options.zstdLevel =
                    std::stoi(requireValue(i, argc, argv));
            } else if (argument == "--board") {
                const std::string value =
                    requireValue(i, argc, argv);

                if (value == "brazilian" || value == "8") {
                    options.boardSize = 8;
                } else if (
                    value == "international" ||
                    value == "10") {
                    options.boardSize = 10;
                } else {
                    throw std::runtime_error(
                        "Unknown board type: " + value);
                }
            } else if (argument == "--store-captures") {
                options.elideCaptures = false;
            } else if (argument == "--keep-temp") {
                options.keepTemporaryFiles = true;
            } else if (argument == "--force") {
                options.force = true;
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

        if (options.outputDirectory.empty()) {
            usage();
            return 2;
        }

        dwtb::generateTablebases(options);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "dwtb-generate: " << e.what() << '\n';
        return 1;
    }
}
