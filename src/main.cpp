#include "wld.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace draughts;

namespace {

void usage(const char* program) {
    std::cerr
        << "Usage:\n"
        << "  " << program
        << " generate --variant brazilian|international"
        << " --pieces N --output PREFIX [--threads N]\n\n"
        << "Examples:\n"
        << "  " << program
        << " generate --variant brazilian"
        << " --pieces 4 --output tables/brazilian"
        << " --threads 16\n"
        << "  " << program
        << " generate --variant international"
        << " --pieces 3 --output tables/international\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2 || std::string(argv[1]) != "generate") {
            usage(argv[0]);
            return 2;
        }

        GenerateOptions options;

        for (int i = 2; i < argc; ++i) {
            const std::string argument = argv[i];

            auto require_value = [&]() -> std::string {
                if (i + 1 >= argc) {
                    throw std::invalid_argument(
                        "missing value after " + argument
                    );
                }
                return argv[++i];
            };

            if (argument == "--variant") {
                const std::string value = require_value();
                if (value == "brazilian") {
                    options.variant = Variant::Brazilian;
                } else if (value == "international") {
                    options.variant = Variant::International;
                } else {
                    throw std::invalid_argument(
                        "unknown variant: " + value
                    );
                }
            } else if (argument == "--pieces") {
                options.maximum_pieces =
                    static_cast<unsigned>(
                        std::stoul(require_value())
                    );
            } else if (argument == "--threads") {
                options.threads =
                    static_cast<unsigned>(
                        std::stoul(require_value())
                    );
            } else if (argument == "--output") {
                options.output_prefix = require_value();
            } else {
                throw std::invalid_argument(
                    "unknown argument: " + argument
                );
            }
        }

        generate_tablebases(options);
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "error: " << exception.what() << '\n';
        return 1;
    }
}
