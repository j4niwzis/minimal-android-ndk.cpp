export module mandk.search;

import std;
import mandk.log;
import mandk.text;

export namespace mandk::search {

// A checkout is walked rather than indexed by a remembered layout. The paths
// in the old NDK-building instructions -- bionic/libc/arch-arm64/include,
// frameworks/native/include and the rest -- do not all exist in a modern
// platform revision, and a rule written against them fails silently by
// copying nothing.
[[nodiscard]] inline std::vector<std::filesystem::path>
walk(const std::filesystem::path &root, bool wantDirectories) {
  std::vector<std::filesystem::path> found;
  std::error_code code;
  auto iterator = std::filesystem::recursive_directory_iterator(
      root, std::filesystem::directory_options::skip_permission_denied, code);
  if (code) {
    return found;
  }
  for (auto entry = iterator; entry != std::filesystem::end(iterator); ++entry) {
    const std::filesystem::path &path = entry->path();
    if (path.filename() == ".git") {
      entry.disable_recursion_pending();
      continue;
    }
    const bool directory = entry->is_directory(code);
    if (directory == wantDirectories) {
      found.push_back(path);
    }
  }
  return found;
}

// Every directory whose path ends with the given suffix. The suffix is
// matched on whole path components, so "libc/include" does not match
// "mylibc/include".
[[nodiscard]] inline std::vector<std::filesystem::path>
directoriesEndingWith(const std::filesystem::path &root,
                      std::string_view suffix) {
  const std::string tail = std::format("/{}", suffix);
  std::vector<std::filesystem::path> matches;
  for (const auto &path : walk(root, true)) {
    if (text::endsWith(path.generic_string(), tail)) {
      matches.push_back(path);
    }
  }
  // Shortest first: the copy of a header set that sits nearest the top of the
  // repository is the one the repository publishes, and a deeper one is
  // usually a test or a vendored duplicate.
  std::ranges::sort(matches, [](const auto &left, const auto &right) {
    const std::string leftText = left.generic_string();
    const std::string rightText = right.generic_string();
    return std::pair(leftText.size(), leftText) <
           std::pair(rightText.size(), rightText);
  });
  return matches;
}

[[nodiscard]] inline std::optional<std::filesystem::path>
directory(const std::filesystem::path &root, std::string_view suffix) {
  const std::vector<std::filesystem::path> matches =
      directoriesEndingWith(root, suffix);
  if (matches.empty()) {
    return std::nullopt;
  }
  if (matches.size() > 1) {
    log::debug("{} matches {} directories under {}; taking {}", suffix,
               matches.size(), root.string(), matches.front().string());
  }
  return matches.front();
}

[[nodiscard]] inline std::vector<std::filesystem::path>
filesNamed(const std::filesystem::path &root, std::string_view name) {
  std::vector<std::filesystem::path> matches;
  for (const auto &path : walk(root, false)) {
    if (path.filename() == name) {
      matches.push_back(path);
    }
  }
  std::ranges::sort(matches, [](const auto &left, const auto &right) {
    const std::string leftText = left.generic_string();
    const std::string rightText = right.generic_string();
    return std::pair(leftText.size(), leftText) <
           std::pair(rightText.size(), rightText);
  });
  return matches;
}

[[nodiscard]] inline std::optional<std::filesystem::path>
file(const std::filesystem::path &root, std::string_view name) {
  const std::vector<std::filesystem::path> matches = filesNamed(root, name);
  if (matches.empty()) {
    return std::nullopt;
  }
  if (matches.size() > 1) {
    log::warn("{} appears {} times under {}; taking {}", name, matches.size(),
              root.string(), matches.front().string());
    for (const auto &path : matches) {
      log::debug("  candidate {}", path.string());
    }
  }
  return matches.front();
}

} // namespace mandk::search
