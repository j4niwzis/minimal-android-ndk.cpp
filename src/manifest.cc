export module mandk.manifest;

import std;
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
