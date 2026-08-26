export module mandk.steps.sources;

import std;
import mandk.git;
import mandk.layout;
import mandk.log;
import mandk.manifest;
import mandk.plan;
import mandk.search;
import mandk.process;

export namespace mandk::steps {

[[nodiscard]] inline Step sources() {
  Step step;
  step.fName = "sources";
  step.fSummary = "check out every pinned repository";
  step.fKey = [](const Context &context) -> std::optional<std::string> {
    return context.baseKey();
  };
  step.fRun = [](Context &context) {
    const std::vector<std::string> unpinned =
        context.fManifest.unpinned(context.fFeatures);
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
      if (!context.wants(source.fFeature)) {
        log::debug("{} belongs to {}, which is off", source.fName,
                   source.fFeature);
        continue;
      }
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

// An NDK-shaped tree made of symlinks, which is what lets Skia's own build
// files work unchanged. Skia's Android toolchain reads its compiler out of
// $ndk/toolchains/llvm/prebuilt/$ndk_host/bin and its sysroot out of the
// directory beside it, and third_party/cpu-features compiles
// $ndk/sources/android/cpufeatures/cpu-features.c. Nothing is copied: the
// compiler here is the host's, and the sysroot is the one this tool built.
[[nodiscard]] inline bool linkTool(const std::filesystem::path &bin,
                                   std::string_view name,
                                   std::string_view program) {
  const std::filesystem::path resolved = findProgram(program);
  if (resolved.empty()) {
    log::debug("{} is not on PATH; the fake NDK will not offer {}", program,
               name);
    return false;
  }
  std::error_code code;
  const std::filesystem::path link = bin / name;
  std::filesystem::remove(link, code);
  std::filesystem::create_symlink(std::filesystem::absolute(resolved, code),
                                  link, code);
  if (code) {
    log::error("cannot link {}: {}", link.string(), code.message());
    return false;
  }
  return true;
}

[[nodiscard]] inline Step ndkCompat() {
  Step step;
  step.fName = "ndk-compat";
  step.fSummary = "an NDK-shaped tree of symlinks for builds that expect one";
  step.fNeeds = {"sources", "sysroot-headers"};
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

    const std::filesystem::path compat = context.fLayout.ndkCompat();
    const std::filesystem::path features =
        compat / "sources" / "android" / "cpufeatures";
    std::optional<std::filesystem::path> source;
    for (const Source &candidate : context.fManifest.fSources) {
      source = search::file(context.fLayout.sourceOf(candidate.fName),
                            "cpu-features.c");
      if (source) {
        break;
      }
    }
    if (!source) {
      if (!context.wants("graphics")) {
        log::debug("no cpu-features.c, and nothing that needs it is on");
      } else {
        log::error("cpu-features.c is in none of the checkouts; Skia's "
                   "Android build compiles it");
        return false;
      }
    }
    if (source) {
      std::filesystem::create_directories(features, code);
      for (const std::string_view name : {"cpu-features.c", "cpu-features.h"}) {
        const std::filesystem::path from = source->parent_path() / name;
        if (!std::filesystem::exists(from, code)) {
          continue;
        }
        std::filesystem::copy_file(
            from, features / name,
            std::filesystem::copy_options::overwrite_existing, code);
        if (code) {
          log::error("cannot copy {}: {}", from.string(), code.message());
          return false;
        }
      }
      log::info("cpu-features: {}", features.string());
    }

    // Skia decides this name from the host operating system alone and calls
    // it linux-x86_64 whatever the host architecture is. The other spelling
    // is there for anything that works the name out honestly.
    const std::filesystem::path prebuilt =
        compat / "toolchains" / "llvm" / "prebuilt";
    const std::filesystem::path host = prebuilt / "linux-x86_64";
    const std::filesystem::path bin = host / "bin";
    std::filesystem::create_directories(bin, code);
    (void)linkTool(bin, "clang", context.fTools.fClang);
    (void)linkTool(bin, "clang++", context.fTools.fClangxx);
    for (const auto &[name, program] :
         std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"ar", context.fTools.fAr},
             {"llvm-ar", context.fTools.fAr},
             {"ranlib", context.fTools.fRanlib},
             {"llvm-ranlib", context.fTools.fRanlib},
             {"readelf", context.fTools.fReadelf},
             {"llvm-readelf", context.fTools.fReadelf},
             {"ld.lld", "ld.lld"},
             {"lld", "lld"},
             {"strip", "llvm-strip"},
             {"llvm-strip", "llvm-strip"},
             {"nm", "llvm-nm"},
             {"objcopy", "llvm-objcopy"},
             {"objdump", "llvm-objdump"}}) {
      (void)linkTool(bin, name, program);
    }
    const std::filesystem::path sysrootLink = host / "sysroot";
    std::filesystem::remove(sysrootLink, code);
    std::filesystem::create_directory_symlink(context.fLayout.sysroot(),
                                              sysrootLink, code);
    const std::filesystem::path other = prebuilt / "linux-aarch64";
    std::filesystem::remove(other, code);
    std::filesystem::create_directory_symlink(host, other, code);
    log::info("fake NDK: {}", compat.string());
    return true;
  };
  return step;
}

} // namespace mandk::steps
