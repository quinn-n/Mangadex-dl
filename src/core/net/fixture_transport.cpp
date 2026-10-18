// SPDX-License-Identifier: GPL-3.0-or-later
//
// The fixture transport (net/fixture_transport.hpp): serves file://<root>/... from a directory
// tree, opening every component with O_NOFOLLOW relative to its parent, so no URL reaches
// outside <root> (no `..`, no symlinks, no absolute re-rooting).
#include "mangadex_dl/net/fixture_transport.hpp"

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/fs/output_dir.hpp"
#include "mangadex_dl/net/transport.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace mangadex_dl {
namespace {

inline constexpr std::string_view file_scheme = "file://";
inline constexpr std::size_t read_chunk = std::size_t{64} * 1024;

[[nodiscard]] std::string errno_text(int err) { return std::generic_category().message(err); }

[[nodiscard]] TransportError bad_url(std::string_view url, std::string_view why) {
    return TransportError{
        .kind = TransportErrc::local,
        .message = std::format("fixture: {}: {}", url, why),
    };
}

/// openat(2) for flags without O_CREAT/O_TMPFILE: the variadic mode argument is never read.
/// `name` is a std::string, so it is NUL-terminated.
[[nodiscard]] int open_at(int dir, const std::string& name, int flags) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): openat is variadic only for the mode
    return ::openat(dir, name.c_str(), flags);
}

[[nodiscard]] HttpResponse status_only(int status) { return HttpResponse{.status = status}; }

/// ENOENT and ENOTDIR are "not in the tree". ELOOP (or ENOTDIR, for an intermediate component
/// opened with O_DIRECTORY) is a symlink component: refused the same way.
[[nodiscard]] bool is_not_found(int err) noexcept {
    return err == ENOENT || err == ENOTDIR || err == ELOOP;
}

/// The value of the first `offset` parameter of `query`: nullopt when there is none, an empty
/// string when it is not a plain decimal number (rejected by the caller).
[[nodiscard]] std::optional<std::string_view> offset_param(std::string_view query) noexcept {
    while (!query.empty()) {
        const auto amp = query.find('&');
        const std::string_view param = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
        constexpr std::string_view key = "offset=";
        if (param.starts_with(key)) {
            const std::string_view value = param.substr(key.size());
            const bool digits = !value.empty() && value.size() <= 18 &&
                                value.find_first_not_of("0123456789") == std::string_view::npos;
            return digits ? value : std::string_view{};
        }
    }
    return std::nullopt;
}

/// Splits `path` on '/' into owned components. False when a component is empty, `.` or `..`.
[[nodiscard]] bool split_components(std::string_view path, std::vector<std::string>& out) {
    while (true) {
        const auto slash = path.find('/');
        const std::string_view comp = path.substr(0, slash);
        if (comp.empty() || comp == "." || comp == "..") {
            return false;
        }
        out.emplace_back(comp);
        if (slash == std::string_view::npos) {
            return true;
        }
        path.remove_prefix(slash + 1);
    }
}

class FixtureTransport final : public Transport {
public:
    FixtureTransport(UniqueFd root_fd, std::string root_prefix) noexcept
        : root_fd_(std::move(root_fd)), root_prefix_(std::move(root_prefix)) {}

    [[nodiscard]] TransportResult fetch(const HttpRequest& request) override {
        if (request.cancel.stop_requested()) {
            return std::unexpected(
                TransportError{.kind = TransportErrc::cancelled, .message = "cancelled"});
        }
        if (request.method == HttpMethod::post) {
            return status_only(404); // fixture mode sends no reports
        }
        auto components = map_url(request.url);
        if (!components.has_value()) {
            return std::unexpected(std::move(components.error()));
        }
        return read_file(*components, request);
    }

private:
    /// file://<root>/<rel>[?query] -> the components of the file to read under the root.
    [[nodiscard]] std::expected<std::vector<std::string>, TransportError>
    map_url(std::string_view url) const {
        if (url.contains('\0')) {
            // Not echoed: a NUL byte must never reach a diagnostic line.
            return std::unexpected(TransportError{.kind = TransportErrc::local,
                                                  .message = "fixture: URL contains a NUL byte"});
        }
        if (url.contains('#')) {
            return std::unexpected(bad_url(url, "fragments are not supported"));
        }
        if (!url.starts_with(file_scheme)) {
            return std::unexpected(bad_url(url, "not a file:// URL"));
        }
        std::string_view rest = url.substr(file_scheme.size());
        const auto question = rest.find('?');
        const std::string_view query =
            question == std::string_view::npos ? std::string_view{} : rest.substr(question + 1);
        rest = rest.substr(0, question);
        if (!rest.starts_with(root_prefix_) || rest.size() <= root_prefix_.size() ||
            rest[root_prefix_.size()] != '/') {
            return std::unexpected(bad_url(url, "outside the fixture root"));
        }
        rest.remove_prefix(root_prefix_.size() + 1);

        std::vector<std::string> components;
        if (!split_components(rest, components)) {
            return std::unexpected(bad_url(url, "empty, '.' or '..' path segment"));
        }
        if (components.size() >= 2 && components.front() == "api") {
            const auto offset = offset_param(query);
            if (offset.has_value()) {
                if (offset->empty()) {
                    return std::unexpected(bad_url(url, "offset is not a decimal number"));
                }
                components.push_back(std::format("offset-{}.json", *offset));
            } else {
                components.back() += ".json";
            }
        }
        return components;
    }

    /// Walks `components` from the root fd. Each intermediate directory is opened O_PATH with
    /// O_DIRECTORY|O_NOFOLLOW and closed (RAII) once its child is open.
    [[nodiscard]] TransportResult read_file(const std::vector<std::string>& components,
                                            const HttpRequest& request) const {
        UniqueFd current{};
        int dir = root_fd_.get();
        for (std::size_t i = 0; i + 1 < components.size(); ++i) {
            const int fd =
                open_at(dir, components[i], O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) {
                return open_failure(errno, request.url);
            }
            current.reset(fd); // closes the previous intermediate directory
            dir = current.get();
        }
        // O_NONBLOCK: opening a FIFO planted in the tree must not block the caller.
        const UniqueFd file{
            open_at(dir, components.back(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
        if (!file.valid()) {
            return open_failure(errno, request.url);
        }
        current.reset();

        struct stat st{};
        if (::fstat(file.get(), &st) != 0) {
            const int err = errno;
            return std::unexpected(TransportError{
                .kind = TransportErrc::local,
                .message = std::format("fixture: {}: fstat: {}", request.url, errno_text(err)),
            });
        }
        if (!S_ISREG(st.st_mode)) {
            return status_only(404); // a directory, FIFO or device is not a fixture document
        }
        return read_body(file.get(), request);
    }

    [[nodiscard]] static TransportResult open_failure(int err, std::string_view url) {
        if (is_not_found(err)) {
            return status_only(404);
        }
        return std::unexpected(TransportError{
            .kind = TransportErrc::local,
            .message = std::format("fixture: {}: {}", url, errno_text(err)),
        });
    }

    /// Reads the whole file, but never more than max_body_bytes (0 refuses any non-empty body).
    [[nodiscard]] static TransportResult read_body(int fd, const HttpRequest& request) {
        HttpResponse response{.status = 200};
        std::array<char, read_chunk> buffer{};
        while (true) {
            const ::ssize_t got = ::read(fd, buffer.data(), buffer.size());
            if (got < 0) {
                const int err = errno;
                if (err == EINTR) {
                    continue;
                }
                return std::unexpected(TransportError{
                    .kind = TransportErrc::local,
                    .message = std::format("fixture: {}: read: {}", request.url, errno_text(err)),
                });
            }
            if (got == 0) {
                return response;
            }
            const auto count = static_cast<std::size_t>(got);
            if (count > request.max_body_bytes - response.body.size()) {
                return std::unexpected(TransportError{
                    .kind = TransportErrc::too_large,
                    .message = std::format("fixture: {}: body exceeds {} bytes", request.url,
                                           request.max_body_bytes),
                });
            }
            response.body.append(buffer.data(), count);
        }
    }

    UniqueFd root_fd_{};        ///< O_PATH directory; only read after construction
    std::string root_prefix_{}; ///< the root as it appears in URLs, without a trailing '/'
};

} // namespace

// NOLINTNEXTLINE(performance-unnecessary-value-param): the by-value signature is frozen (T1 header)
Result<std::unique_ptr<Transport>> make_fixture_transport(std::filesystem::path root) {
    std::string text = root.lexically_normal().string();
    if (!root.is_absolute() || text.contains('\0')) {
        return std::unexpected(Error{
            .code = Errc::failure,
            .message = std::format("fixture root '{}' is not an absolute path", root.string()),
        });
    }
    // The URL form has no trailing '/': "/a/b/" becomes "/a/b", and "/" becomes "".
    while (!text.empty() && text.back() == '/') {
        text.pop_back();
    }
    const std::string open_name = text.empty() ? std::string{"/"} : text;
    UniqueFd fd{open_at(AT_FDCWD, open_name, O_PATH | O_DIRECTORY | O_CLOEXEC)};
    if (!fd.valid()) {
        const int err = errno;
        return std::unexpected(Error{
            .code = Errc::failure,
            .message = std::format("cannot open fixture root '{}': {}", open_name, errno_text(err)),
        });
    }
    return std::make_unique<FixtureTransport>(std::move(fd), std::move(text));
}

} // namespace mangadex_dl
