export module mandk.toolchainfile;

import std;
import mandk.layout;
import mandk.log;
import mandk.plan;

export namespace mandk {

// The CMake toolchain file every cross-built dependency is configured with,
// written out rather than kept in the repository: it states the paths of the
// tree it belongs to and the host programs this run was told to use.
[[nodiscard]] inline std::filesystem::path
toolchainFilePath(const Layout &layout) {
  return layout.root() / "cmake" / "target.cmake";
}

[[nodiscard]] inline bool writeToolchainFile(const Context &context) {
  const Layout &layout = context.fLayout;
  const Target &target = context.target();
  const std::filesystem::path path = toolchainFilePath(layout);
  std::error_code code;
  std::filesystem::create_directories(path.parent_path(), code);
  std::ofstream file(path, std::ios::trunc);
  if (!file) {
    log::error("cannot write {}", path.string());
    return false;
  }
  // Whether this sysroot can link a program.
  //
  // Every CMake check of the form "does this function exist" links one, and
  // a sysroot without bionic's executable startup objects cannot -- so those
  // checks are told to stop at an archive, where an undefined symbol is not
  // an error and the answer is yes to everything. The crt step builds those
  // objects when it can, and this is where that becomes a fact about the
  // build rather than a hope.
  const std::filesystem::path lib = layout.sysrootLib(target);
  std::error_code exists;
  const bool linkable =
      std::filesystem::exists(lib / "crtbegin_dynamic.o", exists) &&
      std::filesystem::exists(lib / "crtend_android.o", exists);
  const std::string probes =
      linkable ? std::string()
               : std::string("# Nothing in this sysroot can link an "
                             "executable: bionic's startup objects\n"
                             "# for one are not in it. Every compiler probe "
                             "therefore has to stop\n"
                             "# at an archive, where an undefined symbol is "
                             "not an error -- which\n"
                             "# means every question of the form \"does this "
                             "function exist\" is\n"
                             "# answered yes.\n"
                             "set(CMAKE_TRY_COMPILE_TARGET_TYPE "
                             "STATIC_LIBRARY)\n");
  if (linkable) {
    log::info("probes link a program: this sysroot has the startup objects");
  }

  file << std::format(
      "# Written by minimal-android-ndk. Anything edited here is overwritten.\n"
      "\n"
      "# Not Android: CMake's Android support goes looking for an NDK to\n"
      "# describe, and there is none. The target triple is what makes this a\n"
      "# build for Android, and __ANDROID__ comes from the compiler with it.\n"
      "set(CMAKE_SYSTEM_NAME Linux)\n"
      "set(CMAKE_SYSTEM_PROCESSOR aarch64)\n"
      "set(ANDROID TRUE CACHE BOOL \"Building for Android\" FORCE)\n"
      "\n"
      "set(MANDK_ROOT \"{0}\")\n"
      "set(MANDK_SYSROOT \"{1}\")\n"
      "set(MANDK_PREFIX \"{2}\")\n"
      "set(MANDK_LIBRARY_DIR \"{3}\")\n"
      "set(MANDK_RESOURCE_DIR \"{4}\")\n"
      "\n"
      "set(CMAKE_SYSROOT \"${{MANDK_SYSROOT}}\")\n"
      "set(CMAKE_C_COMPILER {5} CACHE FILEPATH \"C compiler\")\n"
      "set(CMAKE_CXX_COMPILER {6} CACHE FILEPATH \"C++ compiler\")\n"
      "set(CMAKE_C_COMPILER_TARGET \"{7}\")\n"
      "set(CMAKE_CXX_COMPILER_TARGET \"{7}\")\n"
      "find_program(MANDK_AR NAMES {8} llvm-ar{9} REQUIRED)\n"
      "find_program(MANDK_RANLIB NAMES {10} llvm-ranlib{9} REQUIRED)\n"
      "set(CMAKE_AR \"${{MANDK_AR}}\" CACHE FILEPATH \"archiver\" FORCE)\n"
      "set(CMAKE_RANLIB \"${{MANDK_RANLIB}}\" CACHE FILEPATH \"ranlib\" FORCE)\n"
      "\n"
      // One argument, not two: set() with two of them makes a list, and a
      // list becomes a semicolon in the middle of the flags.
      "set(mandk_common \"--sysroot=${{MANDK_SYSROOT}}"
      " -resource-dir=${{MANDK_RESOURCE_DIR}} -D__ANDROID_NDK__\")\n"
      "set(CMAKE_C_FLAGS_INIT \"${{mandk_common}} -fPIC\")\n"
      "# Bionic declares the ctype functions static inline, and a static\n"
      "# inline definition cannot be re-exported from libc++'s std module.\n"
      "# Bionic offers this override for exactly that, and uses it itself.\n"
      "set(CMAKE_CXX_FLAGS_INIT\n"
      "  \"${{mandk_common}} -D__BIONIC_CTYPE_INLINE=inline -fPIC -stdlib=libc++\")\n"
      // Which runtime library a Clang links by default is decided when
      // that Clang is built: Debian's and apt.llvm.org's packages default
      // to libgcc, Alpine's to compiler-rt. There is no libgcc in this
      // sysroot and there is not going to be one -- the builtins here are
      // compiler-rt and the unwinder is libunwind -- so both are said
      // rather than left to the compiler that happens to be running.
      "set(mandk_link\n"
      "  \"${{mandk_common}} -fuse-ld=lld -rtlib=compiler-rt"
      " -unwindlib=libunwind\")\n"
      "set(CMAKE_EXE_LINKER_FLAGS_INIT \"${{mandk_link}}\")\n"
      "set(CMAKE_SHARED_LINKER_FLAGS_INIT \"${{mandk_link}}\")\n"
      "set(CMAKE_MODULE_LINKER_FLAGS_INIT \"${{mandk_link}}\")\n"
      "\n"
      "set(CMAKE_FIND_ROOT_PATH \"${{MANDK_SYSROOT}}\" \"${{MANDK_PREFIX}}\")\n"
      "set(CMAKE_LIBRARY_PATH \"${{MANDK_LIBRARY_DIR}}\" \"${{MANDK_PREFIX}}/lib\")\n"
      "set(CMAKE_INCLUDE_PATH \"${{MANDK_SYSROOT}}/usr/include\"\n"
      "  \"${{MANDK_SYSROOT}}/usr/include/c++/v1\" \"${{MANDK_PREFIX}}/include\")\n"
      "set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n"
      "set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n"
      "set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n"
      "set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n"
      "\n"
      "{11}"
      "\n"
      "# Android keeps the pthread functions in libc, and a probe that only\n"
      "# archives cannot notice that -lpthreads does not exist. So FindThreads\n"
      "# is told the platform fact instead of asked to discover it.\n"
      "set(THREADS_PREFER_PTHREAD_FLAG OFF)\n"
      "set(CMAKE_HAVE_LIBC_PTHREAD TRUE CACHE INTERNAL \"pthread is in libc\" FORCE)\n"
      "set(CMAKE_THREAD_LIBS_INIT \"\" CACHE STRING \"no -lpthread\" FORCE)\n"
      "set(CMAKE_USE_PTHREADS_INIT 1 CACHE INTERNAL \"\" FORCE)\n"
      "\n"
      "# The .pc files in this prefix describe the target, so pkg-config must\n"
      "# read them and nothing from the host.\n"
      "set(ENV{{PKG_CONFIG_LIBDIR}} \"${{MANDK_PREFIX}}/lib/pkgconfig\")\n"
      "set(ENV{{PKG_CONFIG_PATH}} \"\")\n"
      "set(ENV{{PKG_CONFIG_SYSROOT_DIR}} \"\")\n",
      layout.root().string(), layout.sysroot().string(),
      layout.prefix().string(), layout.sysrootLib(target).string(),
      layout.clangResource().string(), context.fTools.fClang,
      context.fTools.fClangxx, target.clangTarget(), context.fTools.fAr,
      context.fTools.fLlvmBin.empty()
          ? std::string()
          : std::format(" HINTS {}", context.fTools.fLlvmBin),
      context.fTools.fRanlib, probes);
  log::info("{}", path.string());
  return true;
}

[[nodiscard]] inline Step toolchainFile() {
  Step step;
  step.fName = "toolchain-file";
  step.fSummary = "write the CMake toolchain file the dependencies use";
  step.fNeeds = {"sysroot-headers", "api-stubs"};
  step.fRun = [](Context &context) { return writeToolchainFile(context); };
  return step;
}

} // namespace mandk
