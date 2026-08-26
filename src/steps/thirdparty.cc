export module mandk.steps.thirdparty;

import std;
import mandk.build;
import mandk.layout;
import mandk.log;
import mandk.manifest;
import mandk.plan;
import mandk.sha256;

export namespace mandk::steps {

// The packages in the order they can be built, which is the order they were
// written in unless one of them says it needs another.
[[nodiscard]] inline std::optional<std::vector<const Package *>>
packageOrder(const Context &context) {
  const Manifest &manifest = context.fManifest;
  std::map<std::string, const Package *> byName;
  for (const Package &package : manifest.fPackages) {
    byName[package.fName] = &package;
  }
  std::vector<const Package *> ordered;
  std::set<std::string> done;
  std::set<std::string> open;
  const auto visit = [&](const Package *package, auto &&self) -> bool {
    if (done.contains(package->fName)) {
      return true;
    }
    if (!open.insert(package->fName).second) {
      log::error("the packages need each other in a circle at {}",
                 package->fName);
      return false;
    }
    for (const std::string &need : package->fNeeds) {
      const auto found = byName.find(need);
      if (found == byName.end()) {
        log::error("{} needs {}, which is not a package", package->fName, need);
        return false;
      }
      // Turning a feature off cannot quietly take a library out from under
      // something that is still being built.
      if (!context.wants(found->second->fFeature)) {
        log::error("{} needs {}, which is in {} -- and {} is off",
                   package->fName, need, found->second->fFeature,
                   found->second->fFeature);
        return false;
      }
      if (!self(found->second, self)) {
        return false;
      }
    }
    open.erase(package->fName);
    done.insert(package->fName);
    ordered.push_back(package);
    return true;
  };
  for (const Package &package : manifest.fPackages) {
    if (!context.wants(package.fFeature)) {
      log::debug("{} belongs to {}, which is off", package.fName,
                 package.fFeature);
      continue;
    }
    if (!visit(&package, visit)) {
      return std::nullopt;
    }
  }
  return ordered;
}

[[nodiscard]] inline bool buildPackage(Context &context,
                                       const Package &package) {
  const std::filesystem::path checkout =
      context.fLayout.sourceOf(package.fSource);
  const std::filesystem::path source =
      package.fSubdirectory.empty() ? checkout
                                    : checkout / package.fSubdirectory;
  std::error_code code;
  if (!std::filesystem::exists(source, code)) {
    log::error("{} has no source at {}", package.fName, source.string());
    return false;
  }
  log::info("{}", package.fName);
  bool built = false;
  if (package.fBuilder == "cmake") {
    built = build::cmakePackage(context, package.fName, source,
                                package.fOptions);
  } else if (package.fBuilder == "autotools") {
    built = build::autotoolsPackage(context, package.fName, source,
                                    package.fOptions);
  } else {
    log::error("{} asks for a builder called {}, which does not exist",
               package.fName, package.fBuilder);
    return false;
  }
  if (!built) {
    return false;
  }
  bool complete = true;
  for (const std::string &produced : package.fProduces) {
    if (!std::filesystem::exists(context.fLayout.prefix() / produced, code)) {
      log::error("{} finished without installing {}", package.fName, produced);
      complete = false;
    }
  }
  return complete;
}

[[nodiscard]] inline Step thirdParty() {
  Step step;
  step.fName = "third-party";
  step.fSummary = "build the dependencies into the prefix, each as a static "
                  "library";
  step.fNeeds = {"runtimes", "toolchain-file"};
  step.fKey = [](const Context &context) -> std::optional<std::string> {
    Sha256 hash;
    hash.update(context.baseKey());
    for (const Package &package : context.fManifest.fPackages) {
      hash.update(package.fName);
      hash.update(package.fBuilder);
      for (const std::string &option : package.fOptions) {
        hash.update(option);
      }
    }
    return hash.hex();
  };
  step.fRun = [](Context &context) {
    const std::optional<std::vector<const Package *>> ordered =
        packageOrder(context);
    if (!ordered) {
      return false;
    }
    for (const Package *package : *ordered) {
      if (!buildPackage(context, *package)) {
        return false;
      }
    }
    return true;
  };
  return step;
}

} // namespace mandk::steps
