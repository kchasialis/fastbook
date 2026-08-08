#pragma once

#include <cerrno>
#include <format>
#include <source_location>
#include <string_view>
#include <system_error>

inline int check(int rc, std::string_view what,
                 std::source_location loc = std::source_location::current()) {
  if (rc < 0) {
    throw std::system_error(
        errno, std::system_category(),
        std::format("{} at {}:{}", what, loc.file_name(), loc.line()));
  }

  return rc;
}