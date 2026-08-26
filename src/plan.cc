export module mandk.plan;

import std;
import mandk.journal;
import mandk.layout;
import mandk.log;
import mandk.manifest;
import mandk.sha256;

export namespace mandk {

struct Options {
  bool fForce = false;
  bool fDryRun = false;
  bool fKeepGoing = false;
  int fJobs = 1;
};

// The host programs the steps call. The host is a Linux distribution, not a
// downloaded toolchain: on an AArch64 host the system Clang is already a
// compiler for the target architecture, and the archive tools are LLVM's own
// rather than invented paths under the tree being built.
struct Tools {
  std::string fClang = "clang";
  std::string fClangxx = "clang++";
  std::string fPython = "python3";
  std::string fReadelf = "llvm-readelf";
  std::string fAr = "llvm-ar";
  std::string fRanlib = "llvm-ranlib";
  std::string fCmake = "cmake";
  std::string fNinja = "ninja";
  std::string fGn = "gn";
  std::string fPkgConfig = "pkg-config";
  // Where llvm-ar and llvm-ranlib are, when they are not on PATH under those
  // names. Alpine keeps them in /usr/lib/llvm<version>/bin.
  std::string fLlvmBin;
};

struct Context {
  Layout fLayout;
  Manifest fManifest;
  Journal fJournal;
  Options fOptions;
  Tools fTools;
  // The features that are on. Anything with no feature of its own is part of
  // the toolchain proper and is always built.
  std::set<std::string> fFeatures;

  [[nodiscard]] bool wants(std::string_view feature) const {
    return feature.empty() || fFeatures.contains(std::string(feature));
  }

  [[nodiscard]] const Target &target() const { return fManifest.fTarget; }

  // Everything a step's key starts from: which platform sources are pinned
  // and what is being built for.
  [[nodiscard]] std::string baseKey() const {
    Sha256 hash;
    hash.update(fManifest.fTarget.fTriple);
    hash.update(std::to_string(fManifest.fTarget.fApi));
    hash.update(fManifest.fTarget.fArch);
    for (const std::string &feature : fFeatures) {
      hash.update(feature);
    }
    for (const Source &source : fManifest.fSources) {
      hash.update(source.fName);
      hash.update(source.fCommit);
    }
    return hash.hex();
  }
};

struct Step {
  std::string fName;
  std::string fSummary;
  std::vector<std::string> fNeeds;
  // A step that belongs to a feature does not run when that feature is off,
  // and is not a failure for not running.
  std::string fFeature;
  // A step that is declared so that the road is visible but does not build
  // anything yet. It refuses to run rather than reporting a success it did
  // not have.
  bool fReady = true;
  // The key this step would finish with, or nothing when its inputs cannot be
  // enumerated, which makes it run every time.
  std::function<std::optional<std::string>(const Context &)> fKey;
  std::function<bool(Context &)> fRun;
};

class Plan {
public:
  void add(Step step) {
    fIndex[step.fName] = fSteps.size();
    fSteps.push_back(std::move(step));
  }

  [[nodiscard]] const Step *find(std::string_view name) const {
    const auto found = fIndex.find(std::string(name));
    return found == fIndex.end() ? nullptr : &fSteps[found->second];
  }
  [[nodiscard]] const std::vector<Step> &steps() const { return fSteps; }

  // The goals and what they need, in an order where nothing runs before
  // something it needs. A step named twice appears once.
  [[nodiscard]] std::optional<std::vector<std::string>>
  order(std::span<const std::string> goals) const {
    std::vector<std::string> ordered;
    std::set<std::string> done;
    std::vector<std::string> path;
    for (const std::string &goal : goals) {
      if (!this->visit(goal, done, path, ordered)) {
        return std::nullopt;
      }
    }
    return ordered;
  }

  [[nodiscard]] std::vector<std::string> allNames() const {
    std::vector<std::string> names;
    for (const Step &step : fSteps) {
      names.push_back(step.fName);
    }
    return names;
  }

  [[nodiscard]] bool run(Context &context,
                         std::span<const std::string> goals) const {
    const std::optional<std::vector<std::string>> ordered = this->order(goals);
    if (!ordered) {
      return false;
    }
    bool allWell = true;
    for (const std::string &name : *ordered) {
      const Step *const step = this->find(name);
      if (step == nullptr) {
        return false;
      }
      if (!context.wants(step->fFeature)) {
        log::debug("{} belongs to {}, which is off", name, step->fFeature);
        continue;
      }
      const std::optional<std::string> key =
          step->fKey ? step->fKey(context) : std::nullopt;
      if (!context.fOptions.fForce && context.fJournal.upToDate(name, key)) {
        log::debug("{} is up to date", name);
        continue;
      }
      log::note("{}: {}", name, step->fSummary);
      if (context.fOptions.fDryRun) {
        continue;
      }
      if (!step->fReady) {
        log::error("{} is not implemented yet", name);
        return false;
      }
      if (!step->fRun || !step->fRun(context)) {
        log::error("{} failed", name);
        allWell = false;
        if (!context.fOptions.fKeepGoing) {
          return false;
        }
        continue;
      }
      context.fJournal.record(name, key);
    }
    return allWell;
  }

private:
  [[nodiscard]] bool visit(const std::string &name, std::set<std::string> &done,
                           std::vector<std::string> &path,
                           std::vector<std::string> &ordered) const {
    if (done.contains(name)) {
      return true;
    }
    if (std::ranges::find(path, name) != path.end()) {
      path.push_back(name);
      log::error("the steps need each other in a circle: {}",
                 [&path] {
                   std::string joined;
                   for (const std::string &part : path) {
                     joined += joined.empty() ? part : " -> " + part;
                   }
                   return joined;
                 }());
      return false;
    }
    const Step *const step = this->find(name);
    if (step == nullptr) {
      log::error("there is no step called {}", name);
      return false;
    }
    path.push_back(name);
    for (const std::string &need : step->fNeeds) {
      if (!this->visit(need, done, path, ordered)) {
        return false;
      }
    }
    path.pop_back();
    done.insert(name);
    ordered.push_back(name);
    return true;
  }

  std::vector<Step> fSteps;
  std::map<std::string, std::size_t> fIndex;
};

} // namespace mandk
