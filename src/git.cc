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

// Brings one commit into a working tree, and no more of the history than that
// commit needs. paths, when given, are the only directories checked out.
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

  if (const std::optional<std::string> current = head(destination);
      current && *current == commit) {
    log::debug("{} is already at {}", destination.string(), commit);
    return true;
  }

  if (!paths.empty()) {
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
  }

  // A sparse checkout still fetches every blob of the commit unless the
  // server is asked for a partial clone, which is the difference between a
  // few megabytes of frameworks/base and all of it.
  std::vector<std::string> fetch{"fetch", "--depth", "1", "--no-tags"};
  if (!paths.empty()) {
    fetch.push_back("--filter=blob:none");
  }
  fetch.push_back("origin");
  fetch.push_back(commit);
  bool fetched = at(destination, fetch).ok();
  if (!fetched && !paths.empty()) {
    log::debug("{} refused a partial clone; fetching whole blobs", url);
    fetched = at(destination, {"fetch", "--depth", "1", "--no-tags", "origin",
                               commit})
                  .ok();
  }
  if (!fetched && !refHint.empty()) {
    log::note("{} refused a fetch by commit; fetching {} instead", url, refHint);
    fetched = at(destination,
                 {"fetch", "--depth", "1", "origin", refHint})
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
  return true;
}

} // namespace mandk::git
