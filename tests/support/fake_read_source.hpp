#pragma once
// tests/support/fake_read_source.hpp: an in-memory ReadSource with fault injection (skip-existing
// plan T5). Header-only.
//
//     mangadex_dl::test::FakeReadSource source{bytes};
//     source.inject({.at_read = 1, .kind = FakeFault::io, .error = EIO}); // read #1 fails
//     const auto index = mangadex_dl::ZipIndex::read(source);              // bind to a named local
//     CHECK(source.reads() == 2);
//
// Each read_exact call is one "read" (numbered from 0). The fake behaves as FdReadSource does on a
// file whose bytes are `data()` and whose recorded size is `size()`:
// - a range outside size() is short_read, without counting as a transfer;
// - a range inside size() but past the end of data() (a reported size larger than the data, as
//   for a file that shrank after inspection) copies what exists, then is short_read;
// - an injected fault at read #k models what the underlying pread did on that call:
//     io           the first transfer returned -1 with `error` (read_exact reports io + errno);
//     zero         the first transfer returned 0 before completion (short_read);
//     short_count  the first transfer returned only `count` bytes (> 0), and the rest followed in
//                  further transfers: read_exact must still succeed with the right bytes;
// - a mutation scheduled after read #k changes data() once that read has returned, as if another
//   program rewrote the file between two reads.
// It records every call (offset and length) and every simulated transfer, so a test can prove
// that nothing was read (or allocated) for a rejected size.

#include "mangadex_dl/attributes.hpp"
#include "mangadex_dl/fs/read_source.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace mangadex_dl::test {

enum class FakeFault {
    io,          ///< the transfer failed with FakeFaultSpec::error
    zero,        ///< the transfer returned 0 (end of file) before the range was filled
    short_count, ///< the transfer returned FakeFaultSpec::count bytes; the rest followed
};

struct FakeFaultSpec {
    std::size_t at_read = 0; ///< which read_exact call (0-based)
    FakeFault kind = FakeFault::io;
    int error = EIO;       ///< for io
    std::size_t count = 1; ///< for short_count: bytes in the first transfer (clamped to >= 1)
};

/// One byte of data() replaced once read #after_read has returned.
struct FakeMutation {
    std::size_t after_read = 0;
    std::uint64_t offset = 0; ///< ignored when outside data()
    std::byte value{};
};

/// One read_exact call as the source saw it.
struct FakeReadCall {
    std::uint64_t offset = 0;
    std::size_t length = 0;

    friend bool operator==(const FakeReadCall&, const FakeReadCall&) = default;
};

class FakeReadSource final : public ReadSource {
public:
    /// Owns a copy of `data`; the reported size is data.size() until report_size() changes it.
    explicit FakeReadSource(std::vector<std::byte> data)
        : data_(std::move(data)), size_(data_.size()) {}

    /// Reports `size` from size() (bigger or smaller than the data, or beyond zip_max_u32).
    void report_size(std::uint64_t size) noexcept { size_ = size; }

    void inject(const FakeFaultSpec& fault) { faults_.push_back(fault); }
    void mutate(const FakeMutation& mutation) { mutations_.push_back(mutation); }

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

    [[nodiscard]] std::expected<void, ReadError> read_exact(std::uint64_t offset,
                                                            std::span<std::byte> out) override {
        const std::size_t index = calls_.size();
        calls_.push_back(FakeReadCall{.offset = offset, .length = out.size()});
        const std::expected<void, ReadError> result = transfer(index, offset, out);
        apply_mutations(index);
        return result;
    }

    /// read_exact calls so far.
    [[nodiscard]] std::size_t reads() const noexcept { return calls_.size(); }
    /// Every read_exact call, in order. Borrows from *this.
    [[nodiscard]] const std::vector<FakeReadCall>&
    calls() const noexcept MANGADEX_DL_LIFETIMEBOUND {
        return calls_;
    }
    /// Simulated pread transfers (a short count adds one per continuation).
    [[nodiscard]] std::size_t transfers() const noexcept { return transfers_; }
    /// The largest length any read_exact asked for (0 before the first).
    [[nodiscard]] std::size_t largest_read() const noexcept {
        std::size_t largest = 0;
        for (const FakeReadCall& call : calls_) {
            largest = std::max(largest, call.length);
        }
        return largest;
    }
    /// The current bytes (after any mutation). Borrows from *this.
    [[nodiscard]] const std::vector<std::byte>& data() const noexcept MANGADEX_DL_LIFETIMEBOUND {
        return data_;
    }

private:
    [[nodiscard]] static std::unexpected<ReadError> short_read() noexcept {
        return std::unexpected(ReadError{.fault = ReadFault::short_read, .error = 0});
    }

    [[nodiscard]] std::optional<FakeFaultSpec> fault_at(std::size_t index) const noexcept {
        for (const FakeFaultSpec& fault : faults_) {
            if (fault.at_read == index) {
                return fault;
            }
        }
        return std::nullopt;
    }

    /// Copies up to `count` bytes at `offset` of data_ into the front of `out`; returns how many
    /// (0 at or past the end of the data, as pread would).
    [[nodiscard]] std::size_t copy_out(std::uint64_t offset, std::span<std::byte> out,
                                       std::size_t count) noexcept {
        ++transfers_;
        if (offset >= data_.size()) {
            return 0;
        }
        const auto start = static_cast<std::size_t>(offset); // < data_.size()
        const std::size_t available = data_.size() - start;
        const std::size_t n = std::min({count, out.size(), available});
        const std::span<const std::byte> from = std::span{data_}.subspan(start, n);
        std::ranges::copy(from, out.begin());
        return n;
    }

    [[nodiscard]] std::expected<void, ReadError> transfer(std::size_t index, std::uint64_t offset,
                                                          std::span<std::byte> out) noexcept {
        if (offset > size_ || out.size() > size_ - offset) {
            return short_read();
        }
        std::size_t first_limit = out.size();
        if (const std::optional<FakeFaultSpec> fault = fault_at(index); fault && !out.empty()) {
            switch (fault->kind) {
            case FakeFault::io:
                ++transfers_;
                return std::unexpected(ReadError{.fault = ReadFault::io, .error = fault->error});
            case FakeFault::zero:
                ++transfers_;
                return short_read();
            case FakeFault::short_count:
                first_limit = std::max<std::size_t>(fault->count, 1);
                break;
            }
        }
        std::uint64_t position = offset;
        std::span<std::byte> rest = out;
        std::size_t limit = first_limit;
        while (!rest.empty()) {
            const std::size_t got = copy_out(position, rest, limit);
            if (got == 0) {
                return short_read(); // the data ends before the reported size
            }
            rest = rest.subspan(got);
            position += got;
            limit = rest.size();
        }
        return {};
    }

    void apply_mutations(std::size_t index) noexcept {
        for (const FakeMutation& mutation : mutations_) {
            if (mutation.after_read == index && mutation.offset < data_.size()) {
                data_[static_cast<std::size_t>(mutation.offset)] = mutation.value;
            }
        }
    }

    std::vector<std::byte> data_{};
    std::uint64_t size_ = 0;
    std::vector<FakeFaultSpec> faults_{};
    std::vector<FakeMutation> mutations_{};
    std::vector<FakeReadCall> calls_{};
    std::size_t transfers_ = 0;
};

} // namespace mangadex_dl::test
