export module mandk.steps.runtimes;

import std;
import mandk.copy;
import mandk.layout;
import mandk.log;
import mandk.plan;
import mandk.process;
import mandk.search;
import mandk.sha256;

export namespace mandk::steps {

// The compiler these two are built with is the host's, aimed at the target.
// Not a toolchain file: a toolchain file for this target names the runtime
// libraries, and these are the runtime libraries.
[[nodiscard]] inline std::vector<std::string>
crossArguments(const Context &context, const std::filesystem::path &prefix) {
  const Target &target = context.target();
  return {
      std::format("-DCMAKE_C_COMPILER={}", context.fTools.fClang),
      std::format("-DCMAKE_CXX_COMPILER={}", context.fTools.fClangxx),
      std::format("-DCMAKE_ASM_COMPILER={}", context.fTools.fClang),
      std::format("-DCMAKE_C_COMPILER_TARGET={}", target.clangTarget()),
      std::format("-DCMAKE_CXX_COMPILER_TARGET={}", target.clangTarget()),
      std::format("-DCMAKE_ASM_COMPILER_TARGET={}", target.clangTarget()),
      std::format("-DCMAKE_SYSROOT={}", context.fLayout.sysroot().string()),
      std::format("-DCMAKE_INSTALL_PREFIX={}", prefix.string()),
      "-DCMAKE_BUILD_TYPE=Release",
      "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
      // Nothing in this sysroot can link an executable: the Android startup
      // objects are not part of it, and every compiler probe would fail on
      // that rather than on what it was asking about.
      "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
  };
}

// A build directory remembers the source directory it was made for, and
// CMake refuses to reuse it for a different one -- rightly, since the cache
// in it is about the other source. What it cannot know is that this
// directory is scratch, so the answer is to throw it away rather than to
// stop and ask somebody to.
inline void forgetMismatchedCache(const std::filesystem::path &build,
                                  const std::filesystem::path &source) {
  const std::filesystem::path cache = build / "CMakeCache.txt";
  std::error_code code;
  if (!std::filesystem::exists(cache, code)) {
    return;
  }
  std::ifstream file(cache);
  std::string line;
  while (std::getline(file, line)) {
    constexpr std::string_view kHome = "CMAKE_HOME_DIRECTORY:INTERNAL=";
    if (!line.starts_with(kHome)) {
      continue;
    }
    const std::filesystem::path was(line.substr(kHome.size()));
    if (std::filesystem::weakly_canonical(was, code) ==
        std::filesystem::weakly_canonical(source, code)) {
      return;
    }
    log::info("{} was configured for {}; starting it again", build.string(),
              was.string());
    file.close();
    std::filesystem::remove_all(build, code);
    return;
  }
}

[[nodiscard]] inline bool
runCmake(const Context &context, std::string_view what,
         const std::filesystem::path &source,
         const std::filesystem::path &build,
         std::vector<std::string> arguments) {
  std::error_code code;
  forgetMismatchedCache(build, source);
  std::filesystem::create_directories(build, code);
  std::vector<std::string> configure{"-S", source.string(), "-B",
                                     build.string(), "-G", "Ninja"};
  configure.insert(configure.end(), arguments.begin(), arguments.end());
  if (!runChecked({.fProgram = context.fTools.fCmake,
                   .fArguments = std::move(configure)},
                  std::format("configuring {}", what))) {
    return false;
  }
  return runChecked(
      {.fProgram = context.fTools.fCmake,
       .fArguments = {"--build", build.string(), "--target", "install", "-j",
                      std::to_string(context.fOptions.fJobs)},
       .fCapture = false},
      std::format("building {}", what));
}

// The builtins: the small routines a compiler emits calls to and does not
// define. libc++ needs one of them by name -- __emutls_get_address -- and
// without it nothing links, with an error that mentions neither compiler-rt
// nor thread-local storage.
[[nodiscard]] inline Step compilerRt() {
  Step step;
  step.fName = "compiler-rt";
  step.fSummary = "build the builtins libc++ is going to need";
  step.fNeeds = {"sysroot-headers", "api-stubs"};
  step.fKey = [](const Context &context) -> std::optional<std::string> {
    const CommandResult version =
        run({.fProgram = context.fTools.fClang, .fArguments = {"--version"}});
    if (!version.ok()) {
      return std::nullopt;
    }
    Sha256 hash;
    hash.update(context.baseKey());
    hash.update(firstLine(version.fOutput));
    return hash.hex();
  };
  step.fRun = [](Context &context) {
    const std::filesystem::path llvm = context.fLayout.sourceOf("llvm-project");
    const std::optional<std::filesystem::path> builtins =
        search::directory(llvm, "compiler-rt/lib/builtins");
    if (!builtins) {
      log::error("compiler-rt/lib/builtins is not in the llvm-project "
                 "checkout; it is what this step builds");
      return false;
    }

    // A resource directory is not only the builtins. It is also where the
    // compiler keeps its own headers -- stddef.h, stdarg.h, the ones no
    // library provides -- and anything given -resource-dir is expected to
    // have them. Ours held an archive and nothing else, so the first
    // #include <stddef.h> in the runtimes found nothing.
    //
    // So the compiler's own is copied in first and the builtins are added to
    // it, rather than the other way round.
    const std::filesystem::path resource = context.fLayout.clangResource();
    const CommandResult own =
        run({.fProgram = context.fTools.fClang,
             .fArguments = {"-print-resource-dir"}});
    if (!own.ok()) {
      log::error("{} will not say where its resource directory is",
                 context.fTools.fClang);
      return false;
    }
    const std::filesystem::path from(firstLine(own.fOutput));
    std::error_code code;
    if (!std::filesystem::is_directory(from, code)) {
      log::error("{} says its resource directory is {}, and that is not a "
                 "directory",
                 context.fTools.fClang, from.string());
      return false;
    }
    CopyReport carried;
    copyTree(from, resource, {}, carried);
    log::info("{} files from {}", carried.fCopied, from.string());
    if (!std::filesystem::exists(resource / "include" / "stddef.h", code)) {
      log::error("{} has no include/stddef.h after copying {}, and a resource "
                 "directory without the compiler's own headers is one nothing "
                 "can compile against",
                 resource.string(), from.string());
      return false;
    }

    std::vector<std::string> arguments =
        crossArguments(context, resource);
    for (const std::string &extra :
         {std::format("-DCMAKE_AR={}", context.fTools.fAr),
          std::format("-DCMAKE_RANLIB={}", context.fTools.fRanlib),
          std::string("-DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON"),
          std::string("-DCOMPILER_RT_BUILD_BUILTINS=ON"),
          std::string("-DCOMPILER_RT_BUILD_SANITIZERS=OFF"),
          std::string("-DCOMPILER_RT_BUILD_XRAY=OFF"),
          std::string("-DCOMPILER_RT_BUILD_LIBFUZZER=OFF"),
          std::string("-DCOMPILER_RT_BUILD_PROFILE=OFF"),
          std::string("-DCOMPILER_RT_BUILD_MEMPROF=OFF"),
          std::string("-DCOMPILER_RT_BUILD_ORC=OFF"),
          std::string("-DCOMPILER_RT_INCLUDE_TESTS=OFF"),
          std::format("-DCOMPILER_RT_INSTALL_PATH={}", resource.string()),
          std::format("-DLLVM_CMAKE_DIR={}",
                      (llvm / "llvm" / "cmake" / "modules").string())}) {
      arguments.push_back(extra);
    }

    if (!runCmake(context, "compiler-rt builtins", *builtins,
                  context.fLayout.buildOf("compiler-rt"),
                  std::move(arguments))) {
      return false;
    }

    // Looked for rather than assumed: which directory and which spelling of
    // the architecture it lands under is a decision compiler-rt makes.
    std::optional<std::filesystem::path> archive;
    for (const auto &entry : search::walk(resource, false)) {
      const std::string name = entry.filename().string();
      if (name.starts_with("libclang_rt.builtins") && name.ends_with(".a")) {
        archive = entry;
        break;
      }
    }
    if (!archive) {
      log::error("compiler-rt built and installed nothing called "
                 "libclang_rt.builtins*.a under {}",
                 resource.string());
      return false;
    }
    log::info("{}", archive->string());
    return true;
  };
  return step;
}

// libc++, libc++abi and libunwind, and the pieces that make `import std`
// work: the module sources and the metadata that says where they are.
[[nodiscard]] inline Step runtimes() {
  Step step;
  step.fName = "runtimes";
  step.fSummary = "build libc++, libc++abi and libunwind, and put them in the "
                  "sysroot";
  step.fNeeds = {"compiler-rt"};
  step.fKey = [](const Context &context) -> std::optional<std::string> {
    const CommandResult version =
        run({.fProgram = context.fTools.fClangxx, .fArguments = {"--version"}});
    if (!version.ok()) {
      return std::nullopt;
    }
    Sha256 hash;
    hash.update(context.baseKey());
    hash.update(firstLine(version.fOutput));
    hash.update(std::string("runtimes"));
    return hash.hex();
  };
  step.fRun = [](Context &context) {
    const std::filesystem::path llvm = context.fLayout.sourceOf("llvm-project");
    const std::filesystem::path source = llvm / "runtimes";
    std::error_code code;
    if (!std::filesystem::exists(source / "CMakeLists.txt", code)) {
      log::error("the llvm-project checkout has no runtimes directory. A "
                 "partial checkout looks like this, and so does a checkout of "
                 "a revision that predates it.");
      return false;
    }

    const std::filesystem::path install = context.fLayout.runtimeInstall();
    // Bionic declares the ctype functions static inline, and a static inline
    // definition cannot be re-exported from libc++'s std module. Bionic
    // offers this override for exactly that.
    const std::string flags =
        std::format("-resource-dir={}", context.fLayout.clangResource().string());
    std::vector<std::string> arguments = crossArguments(context, install);
    for (const std::string &extra :
         {std::format("-DCMAKE_AR={}", context.fTools.fAr),
          std::format("-DCMAKE_RANLIB={}", context.fTools.fRanlib),
          std::format("-DCMAKE_C_FLAGS={}", flags),
          std::format("-DCMAKE_CXX_FLAGS={} -D__BIONIC_CTYPE_INLINE=inline",
                      flags),
          std::string("-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi;libunwind"),
          std::string("-DLIBCXX_ENABLE_SHARED=OFF"),
          std::string("-DLIBCXXABI_ENABLE_SHARED=OFF"),
          std::string("-DLIBUNWIND_ENABLE_SHARED=OFF"),
          std::string("-DLIBCXX_ENABLE_STATIC=ON"),
          std::string("-DLIBCXXABI_ENABLE_STATIC=ON"),
          std::string("-DLIBUNWIND_ENABLE_STATIC=ON"),
          std::string("-DLIBCXX_USE_COMPILER_RT=ON"),
          std::string("-DLIBCXXABI_USE_COMPILER_RT=ON"),
          std::string("-DLIBCXXABI_USE_LLVM_UNWINDER=ON"),
          // The module sources and libc++.modules.json, which is the whole
          // reason this project needs a libc++ of its own.
          std::string("-DLIBCXX_INSTALL_MODULES=ON"),
          // Tests need an executable, and an executable needs startup
          // objects this sysroot does not have. They also need llvm-lit,
          // which a checkout of the runtimes alone does not carry.
          std::string("-DLLVM_INCLUDE_TESTS=OFF"),
          std::string("-DLIBCXX_INCLUDE_TESTS=OFF"),
          std::string("-DLIBCXX_INCLUDE_BENCHMARKS=OFF"),
          std::string("-DLIBCXXABI_INCLUDE_TESTS=OFF"),
          std::string("-DLIBUNWIND_INCLUDE_TESTS=OFF")}) {
      arguments.push_back(extra);
    }

    if (!runCmake(context, "the C++ runtimes", source,
                  context.fLayout.buildOf("runtimes"), std::move(arguments))) {
      return false;
    }

    // Where the compiler looks for them, which is not where they were
    // installed. The layout is the one the notes describe, because that is
    // the layout the toolchain file expects.
    const std::filesystem::path sysroot = context.fLayout.sysroot();
    const std::filesystem::path libraries =
        context.fLayout.sysrootLib(context.target());
    CopyReport report;
    copyTree(install / "include" / "c++" / "v1",
             sysroot / "usr" / "include" / "c++" / "v1", {}, report);
    copyTree(install / "share" / "libc++" / "v1",
             sysroot / "usr" / "share" / "libc++" / "v1", {}, report);
    std::filesystem::create_directories(libraries, code);
    std::size_t archives = 0;
    for (const auto &entry :
         std::filesystem::directory_iterator(install / "lib", code)) {
      if (entry.path().extension() == ".a") {
        std::filesystem::copy_file(
            entry.path(), libraries / entry.path().filename(),
            std::filesystem::copy_options::overwrite_existing, code);
        ++archives;
      }
    }
    // The metadata says where the module sources are, and the compiler is
    // told to read it by path, so it goes where the toolchain file says.
    const std::filesystem::path metadata = install / "lib" / "libc++.modules.json";
    if (std::filesystem::exists(metadata, code)) {
      std::filesystem::copy_file(
          metadata, sysroot / "usr" / "lib" / "libc++.modules.json",
          std::filesystem::copy_options::overwrite_existing, code);
    }

    bool complete = true;
    for (const std::filesystem::path &wanted :
         {sysroot / "usr" / "include" / "c++" / "v1" / "vector",
          sysroot / "usr" / "lib" / "libc++.modules.json",
          sysroot / "usr" / "share" / "libc++" / "v1" / "std.cppm",
          libraries / "libc++.a", libraries / "libc++abi.a",
          libraries / "libunwind.a"}) {
      if (!std::filesystem::exists(wanted, code)) {
        log::error("{} is missing after installing the runtimes",
                   wanted.string());
        complete = false;
      }
    }
    log::info("{} headers and {} archives", report.fCopied, archives);
    return complete;
  };
  return step;
}

} // namespace mandk::steps
