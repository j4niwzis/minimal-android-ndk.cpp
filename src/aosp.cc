export module mandk.aosp;

import std;
import mandk.git;
import mandk.log;
import mandk.process;
import mandk.text;

export namespace mandk::aosp {

inline constexpr std::string_view kManifestUrl =
    "https://android.googlesource.com/platform/manifest";

// The revisions a release manifest names, by project. A release manifest
// states a commit for every project, which is what makes it the right place
// to resolve a platform tag: the tag itself need not exist in each
// repository.
//
// default.xml is generated, so the elements are read by scanning for
// attributes rather than by parsing XML in general. Anything it cannot read
// it leaves out, and the caller reports the project it wanted and did not
// find.
[[nodiscard]] inline std::optional<std::string>
attribute(std::string_view element, std::string_view name) {
  const std::string needle = std::format("{}=\"", name);
  const std::size_t start = element.find(needle);
  if (start == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t from = start + needle.size();
  const std::size_t end = element.find('"', from);
  if (end == std::string_view::npos) {
    return std::nullopt;
  }
  return std::string(element.substr(from, end - from));
}

[[nodiscard]] inline std::map<std::string, std::string>
projects(std::string_view document) {
  std::map<std::string, std::string> revisions;
  std::string fallback;
  std::size_t cursor = 0;
  while ((cursor = document.find('<', cursor)) != std::string_view::npos) {
    const std::size_t end = document.find('>', cursor);
    if (end == std::string_view::npos) {
      break;
    }
    const std::string_view element = document.substr(cursor, end - cursor + 1);
    cursor = end + 1;
    if (text::startsWith(element, "<default")) {
      fallback = attribute(element, "revision").value_or(std::string());
    } else if (text::startsWith(element, "<project")) {
      const std::optional<std::string> name = attribute(element, "name");
      if (!name) {
        continue;
      }
      revisions[*name] = attribute(element, "revision").value_or(fallback);
    }
  }
  return revisions;
}

// The manifest repository is shallow-cloned at the tag, which is a few
// hundred kilobytes, and read from the working tree.
[[nodiscard]] inline std::optional<std::string>
fetchManifest(const std::string &tag, const std::filesystem::path &workspace) {
  const std::optional<std::string> commit =
      git::resolve(std::string(kManifestUrl), std::format("refs/tags/{}", tag));
  if (!commit) {
    log::error("the platform manifest has no tag {}", tag);
    return std::nullopt;
  }
  log::info("platform {} is manifest commit {}", tag, *commit);
  if (!git::materialize(std::string(kManifestUrl), *commit, workspace, {},
                        std::format("refs/tags/{}", tag))) {
    return std::nullopt;
  }
  const std::filesystem::path file = workspace / "default.xml";
  std::ifstream stream(file);
  if (!stream) {
    log::error("the platform manifest has no default.xml");
    return std::nullopt;
  }
  return std::string((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
}

} // namespace mandk::aosp
