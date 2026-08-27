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
  // A checkout nothing needs until it is asked for. The platform
  // repositories are not optional: they are what the toolchain is made of.
  bool fOptional = false;

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

// A dependency built for the target and installed into the prefix. Every one
// of them is a static library with position-independent code; none of them
// builds its own executables.
//
// fProduces is what must be in the prefix afterwards. A package whose build
// quietly skipped the library and installed only headers is a link error
// several packages later.
struct Package {
  std::string fName;
  std::string fSource;
  std::string fBuilder;
  std::string fSubdirectory;
  std::vector<std::string> fOptions;
  std::vector<std::string> fNeeds;
  std::vector<std::string> fProduces;
  std::string fNote;
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
    manifest.fTargetSdk = document.value("target-sdk", manifest.fTargetSdk);
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
      source.fOptional = entry.value("optional", false);
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
    for (const json &entry : document.value("packages", json::array())) {
      Package package;
      package.fName = entry.value("name", std::string());
      package.fSource = entry.value("source", package.fName);
      package.fBuilder = entry.value("builder", std::string("cmake"));
      package.fSubdirectory = entry.value("subdirectory", std::string());
      package.fNote = entry.value("note", std::string());
      for (const json &item : entry.value("options", json::array())) {
        package.fOptions.push_back(item.get<std::string>());
      }
      for (const json &item : entry.value("needs", json::array())) {
        package.fNeeds.push_back(item.get<std::string>());
      }
      for (const json &item : entry.value("produces", json::array())) {
        package.fProduces.push_back(item.get<std::string>());
      }
      if (package.fName.empty()) {
        log::error("a package in {} has no name", path.string());
        return std::nullopt;
      }
      manifest.fPackages.push_back(std::move(package));
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
    document["target-sdk"] = fTargetSdk;
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
      if (source.fOptional) {
        entry["optional"] = true;
      }
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
    document["packages"] = json::array();
    for (const Package &package : fPackages) {
      json entry;
      entry["name"] = package.fName;
      if (package.fSource != package.fName) {
        entry["source"] = package.fSource;
      }
      entry["builder"] = package.fBuilder;
      if (!package.fSubdirectory.empty()) {
        entry["subdirectory"] = package.fSubdirectory;
      }
      if (!package.fOptions.empty()) {
        entry["options"] = package.fOptions;
      }
      if (!package.fNeeds.empty()) {
        entry["needs"] = package.fNeeds;
      }
      if (!package.fProduces.empty()) {
        entry["produces"] = package.fProduces;
      }
      if (!package.fNote.empty()) {
        entry["note"] = package.fNote;
      }
      document["packages"].push_back(std::move(entry));
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
  [[nodiscard]] const Package *package(std::string_view name) const {
    const auto found = std::ranges::find(fPackages, name, &Package::fName);
    return found == fPackages.end() ? nullptr : &*found;
  }

  // Which checkouts a build of these packages reads: the platform ones, and
  // the one behind each package that was asked for. Nothing else is fetched.
  [[nodiscard]] std::set<std::string>
  neededSources(const std::set<std::string> &packages) const {
    std::set<std::string> names;
    for (const Source &source : fSources) {
      if (!source.fOptional) {
        names.insert(source.fName);
      }
    }
    for (const Package &package : fPackages) {
      if (packages.contains(package.fName)) {
        names.insert(package.fSource);
      }
    }
    return names;
  }

  [[nodiscard]] std::vector<std::string>
  unpinned(const std::set<std::string> &sources) const {
    std::vector<std::string> names;
    for (const Source &source : fSources) {
      if (!source.pinned() && sources.contains(source.fName)) {
        names.push_back(source.fName);
      }
    }
    return names;
  }

  // The packages asked for and everything they need, or nothing when a name
  // is not a package or a dependency is missing.
  [[nodiscard]] std::optional<std::set<std::string>>
  closure(std::span<const std::string> requested) const {
    std::set<std::string> chosen;
    const auto take = [&](const std::string &name, auto &&self) -> bool {
      if (chosen.contains(name)) {
        return true;
      }
      const Package *const found = this->package(name);
      if (found == nullptr) {
        log::error("there is no package called {}", name);
        return false;
      }
      chosen.insert(name);
      for (const std::string &need : found->fNeeds) {
        if (!self(need, self)) {
          log::error("  which {} needs", name);
          return false;
        }
      }
      return true;
    };
    for (const std::string &name : requested) {
      if (!take(name, take)) {
        return std::nullopt;
      }
    }
    return chosen;
  }

  Target fTarget;
  std::string fPlatformTag;
  // What an APK says it was built for, which is not the API the code is
  // compiled against.
  int fTargetSdk = 35;
  std::vector<Source> fSources;
  std::vector<StubLibrary> fStubs;
  std::vector<HeaderRule> fHeaders;
  std::vector<Package> fPackages;
  std::filesystem::path fPath;
};

} // namespace mandk
