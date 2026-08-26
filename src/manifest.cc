export module mandk.manifest;

import std;
import mandk.json;
import mandk.layout;
import mandk.log;

export namespace mandk {

// One checked-out repository. fProject is the name the AOSP manifest knows a
// repository by; a source that has one is pinned from a release manifest,
// and a source without one is pinned from its own remote.
//
// A tag that exists in the platform manifest need not exist in every
// repository it names -- android-14.0.0_r75 does not exist in platform/ndk --
// so a tag is never used as a revision. What is stored is a commit.
struct Source {
  std::string fName;
  std::string fUrl;
  std::string fProject;
  std::string fRef;
  std::string fCommit;
  std::vector<std::string> fPaths;
  std::string fNote;

  [[nodiscard]] bool pinned() const { return !fCommit.empty(); }
};

// A platform library the linker needs a stub for. The map file is found by
// name inside the checkout rather than at a fixed path, because those paths
// move between platform revisions.
struct StubLibrary {
  std::string fName;
  // More than one checkout may be named: a map file that has moved between
  // frameworks/native and frameworks/base is looked for in both, in the order
  // given.
  std::vector<std::string> fSources;
  std::string fFile;
  std::string fPath;
  std::vector<std::string> fVerify;

  [[nodiscard]] std::string soname() const {
    return std::format("lib{}.so", fName);
  }
};

// Where a family of headers comes from. fFind is a path suffix, not a path:
// the checkout is searched for a directory whose path ends with it, so a
// header set that has moved under a different parent is still found. fInto is
// where its contents land under the sysroot's include directory.
//
// fRequire is what must exist afterwards. Header layouts change between
// platform revisions, and a rule that silently matched the wrong directory is
// worse than one that says so.
struct HeaderRule {
  std::string fSource;
  std::string fFind;
  std::string fInto;
  // Copy from every directory that matches rather than from the nearest one.
  // The android/ headers are published from several places at once, and a
  // sysroot needs all of them.
  bool fAll = false;
  std::vector<std::string> fExclude;
  std::vector<std::string> fRequire;
};

class Manifest {
public:
  [[nodiscard]] static std::optional<Manifest>
  load(const std::filesystem::path &path) {
    std::ifstream file(path);
    if (!file) {
      log::error("cannot read the manifest at {}", path.string());
      return std::nullopt;
    }
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    json document;
    try {
      document = json::parse(text, nullptr, true, true);
    } catch (const std::exception &error) {
      log::error("{} is not valid JSON: {}", path.string(), error.what());
      return std::nullopt;
    }
    Manifest manifest;
    manifest.fPath = path;
    if (document.contains("platform")) {
      manifest.fPlatformTag = document["platform"].get<std::string>();
    }
    if (document.contains("target")) {
      const json &target = document["target"];
      manifest.fTarget.fTriple =
          target.value("triple", manifest.fTarget.fTriple);
      manifest.fTarget.fArch = target.value("arch", manifest.fTarget.fArch);
      manifest.fTarget.fAbi = target.value("abi", manifest.fTarget.fAbi);
      manifest.fTarget.fApi = target.value("api", manifest.fTarget.fApi);
    }
    for (const json &entry : document.value("sources", json::array())) {
      Source source;
      source.fName = entry.value("name", std::string());
      source.fUrl = entry.value("url", std::string());
      source.fProject = entry.value("project", std::string());
      source.fRef = entry.value("ref", std::string());
      source.fCommit = entry.value("commit", std::string());
      source.fNote = entry.value("note", std::string());
      for (const json &item : entry.value("paths", json::array())) {
        source.fPaths.push_back(item.get<std::string>());
      }
      if (source.fName.empty() || source.fUrl.empty()) {
        log::error("a source in {} has no name or no url", path.string());
        return std::nullopt;
      }
      manifest.fSources.push_back(std::move(source));
    }
    for (const json &entry : document.value("stubs", json::array())) {
      StubLibrary stub;
      stub.fName = entry.value("library", std::string());
      if (entry.contains("source") && entry["source"].is_array()) {
        for (const json &name : entry["source"]) {
          stub.fSources.push_back(name.get<std::string>());
        }
      } else if (entry.contains("source")) {
        stub.fSources.push_back(entry["source"].get<std::string>());
      }
      stub.fFile = entry.value("file", std::string());
      stub.fPath = entry.value("path", std::string());
      for (const json &symbol : entry.value("verify", json::array())) {
        stub.fVerify.push_back(symbol.get<std::string>());
      }
      manifest.fStubs.push_back(std::move(stub));
    }
    for (const json &entry : document.value("headers", json::array())) {
      HeaderRule rule;
      rule.fSource = entry.value("source", std::string());
      rule.fFind = entry.value("find", std::string());
      rule.fInto = entry.value("into", std::string());
      rule.fAll = entry.value("all", false);
      for (const json &name : entry.value("exclude", json::array())) {
        rule.fExclude.push_back(name.get<std::string>());
      }
      for (const json &name : entry.value("require", json::array())) {
        rule.fRequire.push_back(name.get<std::string>());
      }
      if (rule.fSource.empty() || rule.fFind.empty()) {
        log::error("a header rule in {} has no source or nothing to find",
                   path.string());
        return std::nullopt;
      }
      manifest.fHeaders.push_back(std::move(rule));
    }
    return manifest;
  }

  [[nodiscard]] bool save() const {
    json document;
    document["platform"] = fPlatformTag;
    document["target"] = {{"triple", fTarget.fTriple},
                          {"arch", fTarget.fArch},
                          {"abi", fTarget.fAbi},
                          {"api", fTarget.fApi}};
    document["sources"] = json::array();
    for (const Source &source : fSources) {
      json entry;
      entry["name"] = source.fName;
      entry["url"] = source.fUrl;
      if (!source.fProject.empty()) {
        entry["project"] = source.fProject;
      }
      if (!source.fRef.empty()) {
        entry["ref"] = source.fRef;
      }
      entry["commit"] = source.fCommit;
      if (!source.fPaths.empty()) {
        entry["paths"] = source.fPaths;
      }
      if (!source.fNote.empty()) {
        entry["note"] = source.fNote;
      }
      document["sources"].push_back(std::move(entry));
    }
    document["headers"] = json::array();
    for (const HeaderRule &rule : fHeaders) {
      json entry;
      entry["source"] = rule.fSource;
      entry["find"] = rule.fFind;
      entry["into"] = rule.fInto;
      if (rule.fAll) {
        entry["all"] = true;
      }
      if (!rule.fExclude.empty()) {
        entry["exclude"] = rule.fExclude;
      }
      if (!rule.fRequire.empty()) {
        entry["require"] = rule.fRequire;
      }
      document["headers"].push_back(std::move(entry));
    }
    document["stubs"] = json::array();
    for (const StubLibrary &stub : fStubs) {
      json entry;
      entry["library"] = stub.fName;
      if (stub.fSources.size() == 1) {
        entry["source"] = stub.fSources.front();
      } else {
        entry["source"] = stub.fSources;
      }
      if (!stub.fFile.empty()) {
        entry["file"] = stub.fFile;
      }
      if (!stub.fPath.empty()) {
        entry["path"] = stub.fPath;
      }
      if (!stub.fVerify.empty()) {
        entry["verify"] = stub.fVerify;
      }
      document["stubs"].push_back(std::move(entry));
    }
    // Written beside the manifest and moved over it, so an interrupted write
    // cannot leave the pins half replaced.
    const std::filesystem::path temporary = fPath.string() + ".new";
    {
      std::ofstream file(temporary, std::ios::trunc);
      if (!file) {
        log::error("cannot write {}", temporary.string());
        return false;
      }
      file << document.dump(2) << '\n';
    }
    std::error_code code;
    std::filesystem::rename(temporary, fPath, code);
    if (code) {
      log::error("cannot replace {}: {}", fPath.string(), code.message());
      return false;
    }
    return true;
  }

  [[nodiscard]] const Source *source(std::string_view name) const {
    const auto found = std::ranges::find(fSources, name, &Source::fName);
    return found == fSources.end() ? nullptr : &*found;
  }
  [[nodiscard]] std::vector<std::string> unpinned() const {
    std::vector<std::string> names;
    for (const Source &source : fSources) {
      if (!source.pinned()) {
        names.push_back(source.fName);
      }
    }
    return names;
  }

  Target fTarget;
  std::string fPlatformTag;
  std::vector<Source> fSources;
  std::vector<StubLibrary> fStubs;
  std::vector<HeaderRule> fHeaders;
  std::filesystem::path fPath;
};

} // namespace mandk
