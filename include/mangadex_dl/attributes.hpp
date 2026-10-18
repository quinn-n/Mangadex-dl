// SPDX-License-Identifier: GPL-3.0-or-later
//
// Attribute shims: the only macros this project defines (modern-cpp-conventions, rule 10).
#pragma once

// MANGADEX_DL_LIFETIMEBOUND marks a borrow:
// - after the qualifiers of a member function that returns a view or reference into *this:
//     [[nodiscard]] std::string_view str() const noexcept MANGADEX_DL_LIFETIMEBOUND;
// - after a constructor (or function) parameter the object keeps a reference or pointer to:
//     explicit ZipWriter(ByteSink& sink MANGADEX_DL_LIFETIMEBOUND);
// Clang then diagnoses binding the result to a temporary that dies first, for example
// `std::string_view v = MangaId::parse(x)->str();`. GCC has no equivalent attribute, so there it
// expands to nothing; the rule "never bind a view to a temporary" still holds on both compilers.
#ifdef __has_cpp_attribute
#if __has_cpp_attribute(clang::lifetimebound)
#define MANGADEX_DL_LIFETIMEBOUND [[clang::lifetimebound]]
#endif
#endif
#ifndef MANGADEX_DL_LIFETIMEBOUND
#define MANGADEX_DL_LIFETIMEBOUND
#endif
