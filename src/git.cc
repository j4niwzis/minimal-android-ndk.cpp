export module mandk.git;

import std;
import mandk.log;
import mandk.process;
import mandk.text;

export namespace mandk::git {

[[nodiscard]] inline CommandResult at(const std::filesystem::path &repository,
                                      std::vector<std::string> arguments) {
  std::vector<std::string> full{"-C", repository.string()};
  full.insert(full.end(), arguments.begin(), arguments.end());
  return run({.fProgram = "git", .fArguments = std::move(full)});
}

// The commit a ref currently names on the remote. Used to turn a manifest
// entry that says "a tag" into one that says "a commit", which is the only
// form the rest of the tool accepts.
[[nodiscard]] inline std::optional<std::string> resolve(const std::string &url,
                                                        const std::string &ref) {
  const CommandResult result =
      run({.fProgram = "git", .fArguments = {"ls-remote", url, ref}});
  if (!result.ok()) {
    log::error("git ls-remote {} {} failed", url, ref);
    return std::nullopt;
  }
  // A tag can answer twice: the tag object and, as <ref>^{}, the commit it
  // points at. The commit is the one worth pinning.
  std::string plain;
  std::string peeled;
  for (const auto &line : text::split(result.fOutput, '\n')) {
    const std::string_view trimmed = text::trim(line);
    const std::size_t tab = trimmed.find('\t');
    if (tab == std::string_view::npos) {
      continue;
    }
    const std::string object(trimmed.substr(0, tab));
    const std::string_view name = trimmed.substr(tab + 1);
    if (text::endsWith(name, "^{}")) {
      peeled = object;
    } else if (plain.empty()) {
      plain = object;
    }
  }
  const std::string chosen = peeled.empty() ? plain : peeled;
  if (chosen.empty()) {
    log::error("{} has no ref {}", url, ref);
    return std::nullopt;
  }
  return chosen;
}

[[nodiscard]] inline std::optional<std::string>
head(const std::filesystem::path &repository) {
  const CommandResult result = at(repository, {"rev-parse", "HEAD"});
  if (!result.ok()) {
    return std::nullopt;
  }
  return firstLine(result.fOutput);
}

// How much of the disk a checkout took, so that "minimal" is a number rather
// than an intention.
[[nodiscard]] inline std::uintmax_t
diskUsage(const std::filesystem::path &directory) {
  std::uintmax_t total = 0;
  std::error_code code;
  for (const auto &entry : std::filesystem::recursive_directory_iterator(
           directory, std::filesystem::directory_options::skip_permission_denied,
           code)) {
    if (entry.is_regular_file(code)) {
      total += entry.file_size(code);
    }
  }
  return total;
}

[[nodiscard]] inline std::string megabytes(std::uintmax_t bytes) {
  return std::format("{:.1f} MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

// Brings one commit into a working tree and as little else as the server will
// allow: one commit deep, no tags, no submodules, and -- when only part of
// the tree is wanted -- the blobs of that part only.
//
// Fetching a commit by name needs the server to allow it. Not every host
// does, so a failure falls back to fetching the ref the manifest named and
// then looking for the commit in what arrived; that path is reported, because
// it is slower and because it is the one that can fetch more than was asked
// for.
[[nodiscard]] inline bool materialize(const std::string &url,
                                      const std::string &commit,
                                      const std::filesystem::path &destination,
                                      std::span<const std::string> paths,
                                      const std::string &refHint) {
  std::error_code code;
  if (!std::filesystem::exists(destination / ".git", code)) {
    std::filesystem::create_directories(destination, code);
    if (!runChecked({.fProgram = "git",
                     .fArguments = {"init", "--quiet", destination.string()}},
                    "git init")) {
      return false;
    }
    if (!at(destination, {"remote", "add", "origin", url}).ok()) {
      return false;
    }
  } else {
    // The url can change between manifest revisions; the remote follows it.
    (void)at(destination, {"remote", "set-url", "origin", url});
  }

  // Nothing here wants a repository that repacks itself in the background,
  // and nothing here reads a reflog.
  (void)at(destination, {"config", "gc.auto", "0"});
  (void)at(destination, {"config", "core.logAllRefUpdates", "false"});
  (void)at(destination, {"config", "fetch.recurseSubmodules", "false"});
  (void)at(destination, {"config", "protocol.version", "2"});

  if (const std::optional<std::string> current = head(destination);
      current && *current == commit) {
    log::debug("{} is already at {}", destination.string(), commit);
    return true;
  }

  const bool partial = !paths.empty();
  if (partial) {
    if (!at(destination, {"sparse-checkout", "init", "--cone"}).ok()) {
      log::warn("{} does not support sparse checkout; taking the whole tree",
                destination.string());
    } else {
      std::vector<std::string> arguments{"sparse-checkout", "set"};
      arguments.insert(arguments.end(), paths.begin(), paths.end());
      if (!at(destination, std::move(arguments)).ok()) {
        return false;
      }
    }
    // A sparse checkout still fetches every blob of the commit unless the
    // server is asked for a partial clone, which is the difference between a
    // few megabytes of frameworks/base and all of it. The two settings are
    // what let the later lazy fetches work at all.
    (void)at(destination, {"config", "remote.origin.promisor", "true"});
    (void)at(destination,
             {"config", "remote.origin.partialclonefilter", "blob:none"});
  }

  std::vector<std::string> fetch{"fetch", "--depth", "1", "--no-tags",
                                 "--prune", "--no-recurse-submodules"};
  if (partial) {
    fetch.push_back("--filter=blob:none");
  }
  fetch.push_back("origin");
  fetch.push_back(commit);
  bool fetched = at(destination, fetch).ok();
  if (!fetched && partial) {
    log::debug("{} refused a partial clone; fetching whole blobs", url);
    (void)at(destination, {"config", "--unset", "remote.origin.promisor"});
    (void)at(destination,
             {"config", "--unset", "remote.origin.partialclonefilter"});
    fetched = at(destination, {"fetch", "--depth", "1", "--no-tags",
                               "--no-recurse-submodules", "origin", commit})
                  .ok();
  }
  if (!fetched && !refHint.empty()) {
    log::note("{} refused a fetch by commit; fetching {} instead", url, refHint);
    fetched = at(destination, {"fetch", "--depth", "1", "--no-tags",
                               "--no-recurse-submodules", "origin", refHint})
                  .ok();
  }
  if (!fetched) {
    log::error("cannot fetch {} from {}", commit, url);
    return false;
  }

  if (!runChecked({.fProgram = "git",
                   .fArguments = {"-C", destination.string(), "checkout",
                                  "--quiet", "--detach", commit}},
                  std::format("checkout of {} in {}", commit,
                              destination.filename().string()))) {
    return false;
  }
  log::debug("{} is {}{}", destination.filename().string(),
             megabytes(diskUsage(destination)),
             partial ? " (sparse, blobless)" : "");
  return true;
}

} // namespace mandk::git
