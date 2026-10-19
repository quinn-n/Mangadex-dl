// SPDX-License-Identifier: GPL-3.0-or-later
#include "cmd_download_output.hpp"

#include "command.hpp"
#include "io.hpp"
#include "mangadex_dl/model.hpp"
#include "mangadex_dl/report.hpp"
#include "mangadex_dl/text_safety.hpp"
#include "mangadex_dl/version.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <ostream> // IWYU pragma: keep (std::print(std::ostream&, ...) is declared here)
#include <print>
#include <string>
#include <string_view>

namespace mangadex_dl::app {
namespace {

/// Longest progress label (bytes, after terminal_safe); longer ones are cut and marked "...".
/// Keeps the progress line well under 80 columns so '\r' can rewrite it without wrapping.
constexpr std::size_t max_label_bytes = 32;

[[nodiscard]] constexpr bool is_continuation(unsigned char byte) noexcept {
    return byte >= 0x80 && byte <= 0xBF;
}

/// `text` (valid UTF-8) cut to at most `max_bytes` on a code-point boundary, plus "..." when cut.
[[nodiscard]] std::string bounded(std::string text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text;
    }
    std::size_t cut = max_bytes; // text.size() > max_bytes, so text[cut] is in bounds
    while (cut > 0 && is_continuation(static_cast<unsigned char>(text[cut]))) {
        --cut;
    }
    text.resize(cut);
    text += "...";
    return text;
}

/// "Vol. 03", "Vol. 03 Ch. 021", "Ch. 012.5", "Oneshot", ...: which file is being built, without
/// the title (the title is long and is never echoed on stderr).
[[nodiscard]] std::string progress_label(const PlannedEpub& epub) {
    std::string label;
    switch (epub.kind) {
    case EpubKind::volume:
        label = epub.volume ? std::format("Vol. {}", *epub.volume) : std::string{"Volume"};
        break;
    case EpubKind::chapter:
        if (epub.volume && epub.chapter) {
            label = std::format("Vol. {} Ch. {}", *epub.volume, *epub.chapter);
        } else if (epub.chapter) {
            label = std::format("Ch. {}", *epub.chapter);
        } else {
            label = "Chapter";
        }
        break;
    case EpubKind::oneshot:
        label = "Oneshot";
        break;
    case EpubKind::single:
        label = "All chapters";
        break;
    }
    return bounded(terminal_safe(label), max_label_bytes);
}

/// "1 page", "2 pages", "0 pages": a count with its noun in the right number.
[[nodiscard]] std::string count_of(std::uint64_t count, std::string_view singular,
                                   std::string_view plural) {
    return std::format("{} {}", count, count == 1 ? singular : plural);
}

} // namespace

OutputPolicy output_policy(const GlobalOptions& globals, const Io& io) noexcept {
    return OutputPolicy{
        .json = globals.json,
        .quiet = globals.quiet,
        .verbose = globals.verbose,
        .progress = io.stderr_tty && !globals.quiet && !globals.json && !io.env.term_dumb,
        .color_stderr = resolve_color(globals.color, io.env, io.stderr_tty),
    };
}

std::string terminal_safe(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto byte = static_cast<unsigned char>(text[pos]);
        if (byte < 0x80) {
            out += (byte < 0x20 || byte == 0x7F) ? '?' : text[pos];
            ++pos;
            continue;
        }
        const Utf8Decoded decoded = decode_utf8_at(text, pos); // a lead byte >= 0x80
        if (decoded.length == 0) {
            out += '?'; // one '?' per invalid byte, then resynchronize on the next byte
            ++pos;
            continue;
        }
        if (is_unsafe_code_point(decoded.code_point)) {
            out += '?';
        } else {
            out.append(text.substr(pos, decoded.length));
        }
        pos += decoded.length;
    }
    return out;
}

void print_error(Io& io, const OutputPolicy& policy, std::string_view message) {
    std::print(io.err, "{}: {} {}\n", tool_name, paint("error:", Style::red, policy.color_stderr),
               terminal_safe(message));
}

void print_report_json(Io& io, const RunReport& report) {
    // report_to_json already ends with '\n'. Built whole, written once, never colored.
    std::print(io.out, "{}", report_to_json(report));
}

void ConsoleObserver::on_plan(const Plan& plan) {
    if (policy_.quiet || policy_.verbose < 1) {
        return;
    }
    verbose_line(std::format("planned {}; {} skipped", count_of(plan.epubs.size(), "file", "files"),
                             count_of(plan.skipped.size(), "chapter", "chapters")));
    for (const DedupeNote& note : plan.dedupe) {
        std::string dropped;
        for (const std::string& id : note.dropped_ids) {
            if (!dropped.empty()) {
                dropped += ", ";
            }
            dropped += id;
        }
        verbose_line(std::format("{}: kept {} ({}); dropped {}", note.key, note.kept_id, note.rule,
                                 dropped));
    }
}

void ConsoleObserver::on_epub_begin(const PlannedEpub& epub, std::size_t index, std::size_t total) {
    in_epub_ = true;
    label_ = progress_label(epub);
    index_ = index;
    total_epubs_ = total;
    pages_done_ = 0;
    pages_total_ = epub.planned_pages;
    draw_progress();
}

void ConsoleObserver::on_pages(std::size_t done, std::size_t total) {
    pages_done_ = done;
    pages_total_ = total;
    draw_progress();
}

void ConsoleObserver::on_epub_end(const EpubOutcome& outcome) {
    clear_progress();
    in_epub_ = false;
    switch (outcome.status) {
    case EpubStatus::planned:
        if (!policy_.json) {
            std::print(io_->out, "{}\n", outcome.path); // data: the path exactly as built
        }
        if (!policy_.quiet && policy_.verbose >= 1) {
            // `replaced` on a planned entry: the target exists and --overwrite will replace it.
            verbose_line(std::format("would {} {} ({})", outcome.replaced ? "replace" : "write",
                                     outcome.path, count_of(outcome.pages, "page", "pages")));
        }
        break;
    case EpubStatus::written:
        if (!policy_.json) {
            std::print(io_->out, "{}\n", outcome.path);
        }
        if (!policy_.quiet && policy_.verbose >= 1) {
            verbose_line(std::format("wrote {} ({}, {}{})", outcome.path,
                                     count_of(outcome.pages, "page", "pages"),
                                     count_of(outcome.bytes, "byte", "bytes"),
                                     outcome.replaced ? ", replaced the existing file" : ""));
        }
        break;
    case EpubStatus::exists:
        ++exists_;
        if (!policy_.quiet && policy_.verbose >= 1) {
            verbose_line(std::format("skipped {}: it already exists", outcome.path));
        }
        break;
    case EpubStatus::failed:
        print_error(*io_, policy_,
                    std::format("{}{}: {}", row_.prefix, outcome.path,
                                outcome.error.empty() ? std::string_view{"failed"}
                                                      : std::string_view{outcome.error}));
        break;
    }
}

void ConsoleObserver::on_warning(std::string_view message) { warning(message); }

void ConsoleObserver::on_verbose(int level, std::string_view message) {
    if (policy_.quiet || level < 1 || level > policy_.verbose) {
        return;
    }
    verbose_line(message);
}

void ConsoleObserver::finish() {
    clear_progress();
    in_epub_ = false;
    if (exists_ == 0 || exists_reported_ || !row_.exists_notice) {
        return;
    }
    exists_reported_ = true;
    if (exists_ == 1) {
        warning("1 file already exists; pass --overwrite to replace it");
    } else {
        warning(std::format("{} files already exist; pass --overwrite to replace them", exists_));
    }
}

void ConsoleObserver::warning(std::string_view message) {
    if (policy_.quiet) {
        return;
    }
    clear_progress();
    // The row prefix is sanitized together with the message: it may carry FILE as typed.
    std::string text = row_.prefix;
    text += message;
    std::print(io_->err, "{}: {} {}\n", tool_name,
               paint("warning:", Style::yellow, policy_.color_stderr), terminal_safe(text));
    draw_progress();
}

void ConsoleObserver::verbose_line(std::string_view message) {
    clear_progress();
    std::print(io_->err, "{}: {}\n", tool_name, terminal_safe(message));
    draw_progress();
}

void ConsoleObserver::draw_progress() {
    if (!policy_.progress || !in_epub_) {
        return;
    }
    std::string line =
        row_.position > 0 ? std::format("({}/{}) ", row_.position, row_.total) : std::string{};
    line += std::format("[{}/{}] {}: {}/{} pages", index_ + 1, total_epubs_, label_, pages_done_,
                        pages_total_);
    const std::size_t width = line.size();
    // Pad over whatever is left of a longer previous line. Bytes never undercount the columns
    // of UTF-8 text (a 3-byte CJK character is 2 columns), so the padding always covers it.
    if (width < drawn_width_) {
        line.append(drawn_width_ - width, ' ');
    }
    std::print(io_->err, "\r{}", line);
    io_->err.flush();
    drawn_width_ = width;
}

void ConsoleObserver::clear_progress() {
    if (drawn_width_ == 0) {
        return;
    }
    std::print(io_->err, "\r{}\r", std::string(drawn_width_, ' '));
    io_->err.flush();
    drawn_width_ = 0;
}

} // namespace mangadex_dl::app
