// SPDX-License-Identifier: GPL-3.0-or-later
//
// Loading libcurl at runtime (net/curl_api.hpp): dlopen + dlsym, the runtime gate, the CA choice,
// the global-init reference count, the typed setopt/getinfo wrappers and the SIGPIPE guard.
#include "net/curl_api.hpp"

#include "mangadex_dl/error.hpp"
#include "mangadex_dl/net/curl_transport.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <ctime>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <curl/curl.h>
#include <dlfcn.h>
#include <fcntl.h>
// sigset_t, sigtimedwait and pthread_sigmask are POSIX <signal.h>, not <csignal>.
// NOLINTNEXTLINE(modernize-deprecated-headers): POSIX-only declarations
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mangadex_dl::detail {
namespace {

/// Tried in order. Bare sonames only: the dynamic loader's search path decides (never a path
/// from the environment or argv).
inline constexpr std::array<const char*, 2> curl_sonames{"libcurl.so.4", "libcurl-gnutls.so.4"};

inline constexpr std::string_view install_hint =
    "install your distribution's libcurl package (7.85.0 or newer, with TLS)";

[[nodiscard]] Error load_error(std::string_view detail) {
    return Error{
        .code = Errc::failure,
        .message = std::format("libcurl.so.4 (or libcurl-gnutls.so.4) with TLS support is "
                               "required ({}); {}",
                               detail, install_hint),
    };
}

/// "7.85.0" from 0x075500.
[[nodiscard]] std::string version_text(unsigned int num) {
    return std::format("{}.{}.{}", (num >> 16U) & 0xFFU, (num >> 8U) & 0xFFU, num & 0xFFU);
}

/// The last dlerror() text, copied at once (the buffer is per-thread and reused).
[[nodiscard]] std::string last_dl_error() {
    // glibc keeps the dlerror() state per thread; the text is copied before anything else runs.
    // NOLINTNEXTLINE(concurrency-mt-unsafe): per-thread state in glibc
    const char* text = ::dlerror();
    return text == nullptr ? std::string{"unknown dynamic loader error"} : std::string{text};
}

/// The one place a dlsym result becomes a function pointer. `Fn` is a function type.
template <class Fn> [[nodiscard]] Fn* bind_symbol(void* library, const char* name) noexcept {
    // dlsym returns void*; POSIX guarantees the conversion back to the function pointer type.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the single dlsym cast
    return reinterpret_cast<Fn*>(::dlsym(library, name));
}

/// Binds every entry point. The name of the first missing one on failure.
[[nodiscard]] std::expected<CurlFunctions, std::string> bind_all(void* library) noexcept {
    CurlFunctions fns{};
    const auto bind = [library]<class Fn>(Fn*& slot, const char* name) noexcept {
        slot = bind_symbol<Fn>(library, name);
        return slot != nullptr;
    };
    const std::array<std::pair<bool, const char*>, 12> bound{{
        {bind(fns.global_init, "curl_global_init"), "curl_global_init"},
        {bind(fns.global_cleanup, "curl_global_cleanup"), "curl_global_cleanup"},
        {bind(fns.version_info, "curl_version_info"), "curl_version_info"},
        {bind(fns.easy_init, "curl_easy_init"), "curl_easy_init"},
        {bind(fns.easy_cleanup, "curl_easy_cleanup"), "curl_easy_cleanup"},
        {bind(fns.easy_reset, "curl_easy_reset"), "curl_easy_reset"},
        {bind(fns.easy_setopt, "curl_easy_setopt"), "curl_easy_setopt"},
        {bind(fns.easy_perform, "curl_easy_perform"), "curl_easy_perform"},
        {bind(fns.easy_getinfo, "curl_easy_getinfo"), "curl_easy_getinfo"},
        {bind(fns.easy_strerror, "curl_easy_strerror"), "curl_easy_strerror"},
        {bind(fns.slist_append, "curl_slist_append"), "curl_slist_append"},
        {bind(fns.slist_free_all, "curl_slist_free_all"), "curl_slist_free_all"},
    }};
    for (const auto& [ok, name] : bound) {
        if (!ok) {
            return std::unexpected(std::string{name});
        }
    }
    return fns;
}

/// What a successful load produces; lives in a function-local static for the whole process.
struct LoadedLibrary {
    CurlFunctions functions{};
    std::optional<std::string> ca_bundle{};
};

/// Tries one soname: open, bind, check. Never dlcloses (a rejected library stays mapped; harmless,
/// and it keeps symbolization of anything it did intact). The error is one reason, naming
/// `soname`.
[[nodiscard]] std::expected<LoadedLibrary, std::string> try_library(const char* soname) {
    // Clear any stale error so last_dl_error() reports this call.
    static_cast<void>(last_dl_error());
    void* library = ::dlopen(soname, RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        return std::unexpected(last_dl_error()); // glibc's text starts with the soname
    }
    auto functions = bind_all(library);
    if (!functions.has_value()) {
        return std::unexpected(std::format("{}: missing symbol {}", soname, functions.error()));
    }
    const curl_version_info_data* info = functions->version_info(CURLVERSION_NOW);
    if (info == nullptr) {
        return std::unexpected(std::format("{}: no version information", soname));
    }
    if (auto checked = check_runtime(*info); !checked.has_value()) {
        return std::unexpected(std::format("{}: {}", soname, checked.error().message));
    }
    const CaDefaults defaults = ca_defaults_from(*info);
    // A directory is acceptable only as libcurl's own capath; every bundle must be a file.
    const auto readable = [capath = defaults.capath](const std::string& path) {
        return ca_location_readable(path, capath.has_value() && path == *capath);
    };
    return LoadedLibrary{
        .functions = *functions,
        .ca_bundle = select_ca_bundle(defaults, readable),
    };
}

/// The first soname that loads and passes the checks; otherwise every soname's reason, in order.
[[nodiscard]] Result<LoadedLibrary> load_library() {
    std::string reasons{};
    for (const char* soname : curl_sonames) {
        auto library = try_library(soname);
        if (library.has_value()) {
            return std::move(*library);
        }
        if (!reasons.empty()) {
            reasons += "; ";
        }
        reasons += library.error();
    }
    return std::unexpected(load_error(reasons));
}

/// Loaded once (thread-safe static initialization), success or failure; never unloaded. The
/// object is destroyed at exit, which is harmless: it holds only function pointers and a string,
/// and nothing reads it after main() (a CurlApi copies what it needs).
[[nodiscard]] const Result<LoadedLibrary>& loaded_library() {
    static const Result<LoadedLibrary> library = load_library();
    return library;
}

/// Serializes curl_global_init/cleanup and holds the live CurlApi (if any).
struct Registry {
    std::mutex mutex{};
    std::weak_ptr<const CurlApi> live{};
};

/// Immortal on purpose: ~CurlApi locks this mutex, and a CurlApi released during static
/// destruction (a transport parked in some static) must never find it destroyed. Allocated once,
/// never deleted; the static pointer keeps it reachable, so LeakSanitizer stays quiet.
[[nodiscard]] Registry& registry() {
    static auto* const instance = new Registry{};
    return *instance;
}

/// Whether the NULL-terminated protocol list contains `name`.
[[nodiscard]] bool has_protocol(const char* const* protocols, std::string_view name) noexcept {
    if (protocols == nullptr) {
        return false;
    }
    std::size_t count = 0;
    // libcurl hands out a NULL-terminated array; its length is found once, then it is a span.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): NULL-terminated C array
    while (protocols[count] != nullptr) {
        ++count;
    }
    return std::ranges::any_of(std::span{protocols, count},
                               [name](const char* protocol) { return name == protocol; });
}

/// Whether a handle accepts CURLOPT_PROTOCOLS_STR. Runs after curl_global_init; the probe handle
/// never connects, so its cleanup does no socket I/O.
[[nodiscard]] bool accepts_protocols_str(const CurlFunctions& fns) noexcept {
    const std::unique_ptr<CURL, decltype(fns.easy_cleanup)> probe{fns.easy_init(),
                                                                  fns.easy_cleanup};
    return probe != nullptr &&
           fns.easy_setopt(probe.get(), CURLOPT_PROTOCOLS_STR, curl_allowed_protocols) == CURLE_OK;
}

} // namespace

Result<void> check_runtime(const curl_version_info_data& info) {
    const auto failure = [](std::string message) {
        return std::unexpected(Error{.code = Errc::failure, .message = std::move(message)});
    };
    if (info.version_num < curl_min_version_num) {
        return failure(std::format("libcurl {} is too old; {} or newer is required",
                                   version_text(info.version_num),
                                   version_text(curl_min_version_num)));
    }
    constexpr auto ssl_bit = static_cast<unsigned int>(CURL_VERSION_SSL);
    if ((static_cast<unsigned int>(info.features) & ssl_bit) == 0U) {
        return failure("libcurl was built without TLS support");
    }
    if (!has_protocol(info.protocols, "https")) {
        return failure("libcurl does not support https");
    }
    return {};
}

CaDefaults ca_defaults_from(const curl_version_info_data& info) {
    CaDefaults defaults{};
    if (info.age >= CURLVERSION_SEVENTH) {
        if (info.cainfo != nullptr) {
            defaults.cainfo = std::string{info.cainfo};
        }
        if (info.capath != nullptr) {
            defaults.capath = std::string{info.capath};
        }
    }
    return defaults;
}

bool ca_location_readable(const std::string& path, bool directory_ok) noexcept {
    if (path.empty() || path.contains('\0')) {
        return false;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): no O_CREAT, so no mode argument
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct stat st{};
    const bool stat_ok = ::fstat(fd, &st) == 0;
    static_cast<void>(::close(fd));
    if (!stat_ok) {
        return false;
    }
    return S_ISREG(st.st_mode) || (directory_ok && S_ISDIR(st.st_mode));
}

SigpipeGuard::SigpipeGuard() noexcept {
    // NOLINTNEXTLINE(misc-include-cleaner): POSIX sigset_t; glibc defines it in a bits/ header
    sigset_t set{};
    static_cast<void>(sigemptyset(&set));
    static_cast<void>(sigaddset(&set, SIGPIPE));
    blocked_ = pthread_sigmask(SIG_BLOCK, &set, &previous_) == 0;
}

SigpipeGuard::~SigpipeGuard() {
    if (!blocked_) {
        return;
    }
    // NOLINTNEXTLINE(misc-include-cleaner): POSIX sigset_t; glibc defines it in a bits/ header
    sigset_t set{};
    static_cast<void>(sigemptyset(&set));
    static_cast<void>(sigaddset(&set, SIGPIPE));
    // NOLINTNEXTLINE(misc-include-cleaner): POSIX sigset_t; glibc defines it in a bits/ header
    sigset_t pending{};
    if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
        // A standard signal is pending at most once per thread and once per process; each call
        // consumes one. EINTR (another signal handled meanwhile) retries.
        const std::timespec zero{};
        int consumed = 0;
        while (consumed < 2) {
            const int got = sigtimedwait(&set, nullptr, &zero);
            if (got == SIGPIPE) {
                ++consumed;
            } else if (got < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
    }
    static_cast<void>(pthread_sigmask(SIG_SETMASK, &previous_, nullptr));
}

Result<std::shared_ptr<const CurlApi>> CurlApi::load() {
    const Result<LoadedLibrary>& library = loaded_library();
    if (!library.has_value()) {
        return std::unexpected(library.error());
    }
    Registry& reg = registry();
    const std::scoped_lock lock{reg.mutex};
    if (std::shared_ptr<const CurlApi> live = reg.live.lock()) {
        return live;
    }
    const CURLcode rc = library->functions.global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK) {
        const char* text = library->functions.easy_strerror(rc);
        return std::unexpected(Error{
            .code = Errc::failure,
            .message = std::format("libcurl initialization failed: {}",
                                   text == nullptr ? "unknown error" : text),
        });
    }
    if (!accepts_protocols_str(library->functions)) {
        library->functions.global_cleanup(); // give back the reference taken above
        return std::unexpected(load_error("it rejects CURLOPT_PROTOCOLS_STR"));
    }
    std::shared_ptr<const CurlApi> api{};
    try {
        api = std::make_shared<const CurlApi>(Key{}, library->functions, library->ca_bundle);
    } catch (...) {
        library->functions.global_cleanup(); // give back the reference taken above
        throw;
    }
    reg.live = api;
    return api;
}

CurlApi::CurlApi(Key /*key*/, const CurlFunctions& functions, std::optional<std::string> ca_bundle)
    : fns_(functions), ca_bundle_(std::move(ca_bundle)) {}

CurlApi::~CurlApi() {
    // The registry's weak_ptr is already expired, so a concurrent load() starts a new reference;
    // libcurl counts init/cleanup pairs, so the order of the two under the mutex does not matter.
    const std::scoped_lock lock{registry().mutex};
    const SigpipeGuard guard;
    fns_.global_cleanup();
}

CURL* CurlApi::easy_init() const noexcept { return fns_.easy_init(); }

void CurlApi::easy_cleanup(CURL* handle) const noexcept { fns_.easy_cleanup(handle); }

void CurlApi::easy_reset(CURL* handle) const noexcept { fns_.easy_reset(handle); }

CURLcode CurlApi::easy_perform(CURL* handle) const noexcept { return fns_.easy_perform(handle); }

const char* CurlApi::easy_strerror(CURLcode code) const noexcept {
    const char* text = fns_.easy_strerror(code);
    return text == nullptr ? "unknown libcurl error" : text;
}

CURLcode CurlApi::setopt_long(CURL* handle, CURLoption option, long value) const noexcept {
    return fns_.easy_setopt(handle, option, value);
}

CURLcode CurlApi::setopt_off(CURL* handle, CURLoption option, curl_off_t value) const noexcept {
    return fns_.easy_setopt(handle, option, value);
}

CURLcode CurlApi::setopt_str(CURL* handle, CURLoption option, const char* value) const noexcept {
    return fns_.easy_setopt(handle, option, value);
}

CURLcode CurlApi::setopt_borrowed(CURL* handle, CURLoption option,
                                  const char* value) const noexcept {
    // libcurl reads CURLOPT_POSTFIELDS with va_arg(param, void *) and never writes through it;
    // a pointer to (const) void has the same representation.
    return fns_.easy_setopt(handle, option, static_cast<const void*>(value));
}

CURLcode CurlApi::setopt_ptr(CURL* handle, CURLoption option, void* value) const noexcept {
    return fns_.easy_setopt(handle, option, value);
}

CURLcode CurlApi::setopt_slist(CURL* handle, CURLoption option, curl_slist* list) const noexcept {
    return fns_.easy_setopt(handle, option, list);
}

CURLcode CurlApi::setopt_write_cb(CURL* handle, CURLoption option,
                                  curl_write_callback callback) const noexcept {
    return fns_.easy_setopt(handle, option, callback);
}

CURLcode CurlApi::setopt_xfer_cb(CURL* handle, CURLoption option,
                                 curl_xferinfo_callback callback) const noexcept {
    return fns_.easy_setopt(handle, option, callback);
}

CURLcode CurlApi::getinfo_long(CURL* handle, CURLINFO info, long* out) const noexcept {
    return fns_.easy_getinfo(handle, info, out);
}

curl_slist* CurlApi::slist_append(curl_slist* list, const char* value) const noexcept {
    return fns_.slist_append(list, value);
}

void CurlApi::slist_free_all(curl_slist* list) const noexcept { fns_.slist_free_all(list); }

} // namespace mangadex_dl::detail
