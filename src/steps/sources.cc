export module mandk.steps.sources;

import std;
import mandk.git;
import mandk.layout;
import mandk.log;
import mandk.manifest;
import mandk.plan;
import mandk.search;
import mandk.sha256;

export namespace mandk::steps {

[[nodiscard]] inline Step sources() {
  Step step;
  step.fName = "sources";
  step.fSummary = "check out every pinned repository";
  step.fKey = [](const Context &context) -> std::optional<std::string> {
    return context.baseKey();
  };
  step.fRun = [](Context &context) {
    const std::vector<std::string> unpinned = context.fManifest.unpinned();
    if (!unpinned.empty()) {
      log::error("these sources have no commit: {}",
                 [&unpinned] {
                   std::string joined;
                   for (const std::string &name : unpinned) {
                     joined += joined.empty() ? name : ", " + name;
                   }
                   return joined;
                 }());
      log::error("run `minimal-android-ndk pin --platform <android release tag>` first");
      return false;
    }
    for (const Source &source : context.fManifest.fSources) {
      const std::filesystem::path destination =
          context.fLayout.sourceOf(source.fName);
      log::info("{} at {}", source.fName, source.fCommit.substr(0, 12));
      if (!git::materialize(source.fUrl, source.fCommit, destination,
                            source.fPaths, source.fRef)) {
        return false;
      }
    }
    return true;
  };
  return step;
}

// Two files the rest of the build expects to find in an NDK-shaped tree
// rather than where the platform keeps them: the native app glue the
// application links, and the cpu-features source Skia's Android build
// compiles. Both are located by name, because both have moved before.
[[nodiscard]] inline Step ndkCompat() {
  Step step;
  step.fName = "ndk-compat";
  step.fSummary = "put native_app_glue and cpu-features where builds look";
  step.fNeeds = {"sources"};
  step.fKey = [](const Context &context) -> std::optional<std::string> {
    return context.baseKey();
  };
  step.fRun = [](Context &context) {
    const std::filesystem::path ndk = context.fLayout.sourceOf("ndk");
    std::error_code code;
    if (!std::filesystem::exists(ndk, code)) {
      log::error("the ndk checkout is missing; it carries native_app_glue");
      return false;
    }
    const std::optional<std::filesystem::path> glue =
        search::directory(ndk, "sources/android/native_app_glue");
    if (!glue) {
      log::error("native_app_glue is not in the ndk checkout");
      return false;
    }
    log::info("native app glue: {}", glue->string());

    const std::filesystem::path features =
        context.fLayout.ndkCompat() / "sources" / "android" / "cpufeatures";
    std::optional<std::filesystem::path> source;
    for (const Source &candidate : context.fManifest.fSources) {
      source = search::file(context.fLayout.sourceOf(candidate.fName),
                            "cpu-features.c");
      if (source) {
        break;
      }
    }
    if (!source) {
      log::error("cpu-features.c is in none of the checkouts; Skia's Android "
                 "build compiles it");
      return false;
    }
    std::filesystem::create_directories(features, code);
    for (const std::string_view name : {"cpu-features.c", "cpu-features.h"}) {
      const std::filesystem::path from = source->parent_path() / name;
      if (!std::filesystem::exists(from, code)) {
        continue;
      }
      std::filesystem::copy_file(from, features / name,
                                 std::filesystem::copy_options::
                                     overwrite_existing,
                                 code);
      if (code) {
        log::error("cannot copy {}: {}", from.string(), code.message());
        return false;
      }
    }
    log::info("cpu-features: {}", features.string());
    return true;
  };
  return step;
}

} // namespace mandk::steps
