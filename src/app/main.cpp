// SPDX-License-Identifier: GPL-3.0-or-later
//
// The firewall. main() holds no logic: it restores SIGPIPE, runs the CLI, turns any escaped
// exception into an exit code, and flushes and checks stdout. It returns; it never calls std::exit.
// Keep this file to main() alone: tests compile every other src/app/ file into the CLI test binary.
#include "cli.hpp"
#include "io.hpp"
#include "mangadex_dl/error.hpp"
#include "mangadex_dl/exit_codes.hpp"

#include <cstddef>
#include <exception>
#include <span>
#include <system_error>

// NOLINTNEXTLINE(misc-const-correctness): the signature of main is fixed by the standard
int main(int argc, char** argv) {
    mangadex_dl::app::restore_default_sigpipe(); // SIGPIPE policy A
    int rc = mangadex_dl::exit_code::internal;
    try {
        auto io = mangadex_dl::app::process_io();
        const char* const* const argv_view = argv; // qualification conversion, no cast
        const std::span<const char* const> args{argv_view, static_cast<std::size_t>(argc)};
        rc = mangadex_dl::app::run(args, io);
    } catch (const mangadex_dl::Failure& e) {
        mangadex_dl::app::last_resort(e.what());
        rc = e.exit_code();
    } catch (const std::system_error& e) {
        // Before std::exception: FILE*-based std::print and std::filesystem throw this with the
        // real errno (ENOSPC, EPIPE, EACCES...). An expected runtime failure, not a bug.
        mangadex_dl::app::last_resort(e.what());
        rc = mangadex_dl::exit_code::failure;
    } catch (const std::exception& e) {
        mangadex_dl::app::last_resort(e.what());
        rc = mangadex_dl::exit_code::internal;
    } catch (...) {
        mangadex_dl::app::last_resort("unknown exception");
        rc = mangadex_dl::exit_code::internal;
    }
    return mangadex_dl::app::finish_stdout(rc);
}
