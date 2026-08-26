export module mandk.copy;

import std;
import mandk.log;
import mandk.sha256;

export namespace mandk {

struct CopyReport {
  std::size_t fCopied = 0;
  std::size_t fSkipped = 0;
  std::size_t fConflicts = 0;
};

// Copies a tree into a destination that other rules also write into, which is
// what assembling a sysroot out of several repositories is.
//
// A file that is already there is left alone. When the two differ it is
// reported: two repositories publishing different versions of the same header
// is a manifest that names the wrong revision of one of them, and silently
// keeping either one hides that.
inline void copyTree(const std::filesystem::path &from,
                     const std::filesystem::path &into,
                     std::span<const std::string> exclude, CopyReport &report) {
  std::error_code code;
  std::filesystem::create_directories(into, code);
  for (const auto &entry : std::filesystem::recursive_directory_iterator(
           from, std::filesystem::directory_options::skip_permission_denied,
           code)) {
    const std::filesystem::path relative =
        std::filesystem::relative(entry.path(), from, code);
    if (relative.empty()) {
      continue;
    }
    const std::string first = relative.begin()->string();
    if (std::ranges::find(exclude, first) != exclude.end()) {
      continue;
    }
    const std::filesystem::path target = into / relative;
    if (entry.is_directory(code)) {
      std::filesystem::create_directories(target, code);
      continue;
    }
    if (!entry.is_regular_file(code)) {
      continue;
    }
    if (std::filesystem::exists(target, code)) {
      const std::optional<std::string> here = sha256File(entry.path());
      const std::optional<std::string> there = sha256File(target);
      if (here && there && *here != *there) {
        log::warn("{} is claimed twice with different contents",
                  relative.generic_string());
        log::warn("  kept {}", target.string());
        log::warn("  ignored {}", entry.path().string());
        ++report.fConflicts;
      }
      ++report.fSkipped;
      continue;
    }
    std::filesystem::create_directories(target.parent_path(), code);
    std::filesystem::copy_file(
        entry.path(), target,
        std::filesystem::copy_options::overwrite_existing, code);
    if (code) {
      log::warn("cannot copy {}: {}", entry.path().string(), code.message());
      continue;
    }
    ++report.fCopied;
  }
}

} // namespace mandk
