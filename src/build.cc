export module mandk.build;

import std;
import mandk.layout;
import mandk.log;
import mandk.plan;
import mandk.process;
import mandk.text;
import mandk.toolchainfile;

export namespace mandk::build {

// What every dependency is configured against. None of them builds an
// executable: the Android CRT startup objects are not in this link sysroot,
// so a package that insists on its own tools, tests, examples or benchmarks
// fails at the last link rather than at configure time.
[[nodiscard]] inline std::vector<std::string>
commonCmakeOptions(const Context &context, const std::filesystem::path &source) {
  return {
      std::format("-DCMAKE_TOOLCHAIN_FILE={}",
                  toolchainFilePath(context.fLayout).string()),
      std::format("-DCMAKE_INSTALL_PREFIX={}",
                  context.fLayout.prefix().string()),
      std::format("-DCMAKE_PREFIX_PATH={}", context.fLayout.prefix().string()),
      "-DCMAKE_BUILD_TYPE=Release",
      "-DBUILD_SHARED_LIBS=OFF",
      "-DBUILD_TESTING=OFF",
      "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
      std::format("-S{}", source.string()),
  };
}

[[nodiscard]] inline bool cmakePackage(const Context &context,
                                       const std::string &name,
                                       const std::filesystem::path &source,
                                       std::span<const std::string> options) {
  const std::filesystem::path build =
      context.fLayout.buildOf(std::format("prefix/{}", name));
  std::vector<std::string> arguments = commonCmakeOptions(context, source);
  arguments.push_back(std::format("-B{}", build.string()));
  arguments.push_back("-GNinja");
  arguments.insert(arguments.end(), options.begin(), options.end());
  if (!runChecked({.fProgram = context.fTools.fCmake,
                   .fArguments = std::move(arguments)},
                  std::format("configuring {}", name))) {
    return false;
  }
  return runChecked(
      {.fProgram = context.fTools.fCmake,
       .fArguments = {"--build", build.string(), "--target", "install", "-j",
                      std::to_string(context.fOptions.fJobs)}},
      std::format("building {}", name));
}

// The environment an Autotools configure script is run with. The compiler is
// named once, in the environment, because a configure script that is handed
// --host works out the rest of the tool names from it and would otherwise
// look for a prefixed cross compiler that does not exist here.
[[nodiscard]] inline std::vector<std::pair<std::string, std::string>>
autotoolsEnvironment(const Context &context) {
  const std::string flags = std::format(
      "--target={} --sysroot={} -resource-dir={} -fPIC",
      context.target().clangTarget(), context.fLayout.sysroot().string(),
      context.fLayout.clangResource().string());
  return {
      {"CC", context.fTools.fClang},
      {"CXX", context.fTools.fClangxx},
      {"AR", context.fTools.fAr},
      {"RANLIB", context.fTools.fRanlib},
      {"CFLAGS", flags},
      {"CXXFLAGS", flags + " -stdlib=libc++"},
      {"LDFLAGS", std::format("-L{}/lib -L{}", context.fLayout.prefix().string(),
                              context.fLayout.sysrootLib(context.target())
                                  .string())},
      {"PKG_CONFIG_LIBDIR",
       std::format("{}/lib/pkgconfig", context.fLayout.prefix().string())},
      {"PKG_CONFIG_SYSROOT_DIR", ""},
  };
}

[[nodiscard]] inline bool autotoolsPackage(const Context &context,
                                           const std::string &name,
                                           const std::filesystem::path &source,
                                           std::span<const std::string> options) {
  // Autotools rebuilds its own generated files when they look older than
  // their sources, and a fresh checkout has no useful timestamps at all. The
  // generated files are touched in dependency order first, so make leaves
  // them alone instead of demanding a matching Autoconf and Automake.
  std::error_code code;
  const auto now = std::filesystem::file_time_type::clock::now();
  int age = 6;
  for (const std::string_view generated :
       {"configure.ac", "configure.in", "aclocal.m4", "configure",
        "config.h.in", "Makefile.in"}) {
    const std::filesystem::path file = source / generated;
    if (std::filesystem::exists(file, code)) {
      std::filesystem::last_write_time(file, now - std::chrono::seconds(age),
                                       code);
    }
    --age;
  }

  const std::filesystem::path build =
      context.fLayout.buildOf(std::format("prefix/{}", name));
  std::filesystem::create_directories(build, code);
  std::vector<std::string> arguments{
      std::format("--host={}", context.target().fTriple),
      std::format("--prefix={}", context.fLayout.prefix().string()),
      "--disable-shared", "--enable-static"};
  arguments.insert(arguments.end(), options.begin(), options.end());
  if (!runChecked({.fProgram = (source / "configure").string(),
                   .fArguments = std::move(arguments),
                   .fDirectory = build,
                   .fEnvironment = autotoolsEnvironment(context)},
                  std::format("configuring {}", name))) {
    return false;
  }
  if (!runChecked({.fProgram = "make",
                   .fArguments = {"-j", std::to_string(context.fOptions.fJobs)},
                   .fDirectory = build,
                   .fEnvironment = autotoolsEnvironment(context)},
                  std::format("building {}", name))) {
    return false;
  }
  return runChecked({.fProgram = "make",
                     .fArguments = {"install"},
                     .fDirectory = build,
                     .fEnvironment = autotoolsEnvironment(context)},
                    std::format("installing {}", name));
}

// A pkg-config file describing this prefix rather than the host's. Written
// for the packages that do not produce one themselves.
[[nodiscard]] inline bool writePackageConfig(
    const Context &context, const std::string &name, const std::string &version,
    const std::string &requires_, const std::string &libs,
    const std::string &cflags) {
  const std::filesystem::path path = context.fLayout.prefix() / "lib" /
                                     "pkgconfig" / std::format("{}.pc", name);
  std::error_code code;
  std::filesystem::create_directories(path.parent_path(), code);
  std::ofstream file(path, std::ios::trunc);
  if (!file) {
    log::error("cannot write {}", path.string());
    return false;
  }
  file << std::format("prefix={}\n"
                      "exec_prefix=${{prefix}}\n"
                      "libdir=${{prefix}}/lib\n"
                      "includedir=${{prefix}}/include\n"
                      "\n"
                      "Name: {}\n"
                      "Description: built by minimal-android-ndk\n"
                      "Version: {}\n"
                      "Requires: {}\n"
                      "Libs: {}\n"
                      "Cflags: {}\n",
                      context.fLayout.prefix().string(), name, version,
                      requires_, libs, cflags);
  return true;
}

} // namespace mandk::build
