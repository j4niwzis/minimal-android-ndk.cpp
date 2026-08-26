export module mandk.steps.skia;

import std;
import mandk.build;
import mandk.copy;
import mandk.git;
import mandk.layout;
import mandk.log;
import mandk.manifest;
import mandk.plan;
import mandk.process;
import mandk.sha256;
import mandk.text;

export namespace mandk::steps {

// Skia can be told to use the libraries the system has instead of the copies
// in third_party/externals, and here it is: every codec library in the prefix
// is one this tool built for the target. That removes the sync of Skia's
// dependencies altogether, and with it the reason the bundled zlib was a
// problem -- that copy is Chromium's, and its zconf.h includes a chromeconf.h
// which is not part of zlib.
[[nodiscard]] inline std::vector<std::string> gnArguments(const Context &context) {
  const std::string prefix = context.fLayout.prefix().string();
  const std::string compat = context.fLayout.ndkCompat().string();
  return {
      std::format("ndk=\"{}\"", compat),
      std::format("ndk_api={}", context.target().fApi),
      "target_cpu=\"arm64\"",
      "target_os=\"android\"",
      "is_official_build=true",
      "is_component_build=false",
      // Ganesh on GLES. Vulkan wants the whole Vulkan and vk_video header
      // closure, which this sysroot does not have.
      "skia_use_gl=true",
      "skia_use_egl=true",
      "skia_use_vulkan=false",
      "skia_use_dawn=false",
      "skia_use_metal=false",
      // Every one of these is in the prefix, built for the target.
      "skia_use_system_zlib=true",
      "skia_use_system_libpng=true",
      "skia_use_system_libjpeg_turbo=true",
      "skia_use_system_libwebp=true",
      "skia_use_system_freetype2=true",
      "skia_use_freetype=true",
      // Not built, so not asked for from the system either.
      "skia_use_system_harfbuzz=false",
      "skia_use_harfbuzz=false",
      "skia_use_system_expat=false",
      "skia_use_expat=false",
      "skia_use_icu=false",
      "skia_use_wuffs=false",
      "skia_use_dng_sdk=false",
      "skia_use_piex=false",
      "skia_use_fontconfig=false",
      // The Android font manager reads the system font configuration through
      // ICU headers such as unicode/uchar.h. The application supplies its own
      // fonts, so the portable manager is the one that fits.
      "skia_enable_fontmgr_android=false",
      "skia_enable_fontmgr_custom_directory=true",
      "skia_enable_fontmgr_custom_empty=true",
      "skia_enable_tools=false",
      std::format(
          "extra_cflags=[\"-I{0}/include\",\"-I{0}/include/freetype2\","
          "\"-D__BIONIC_CTYPE_INLINE=inline\"]",
          prefix),
      std::format("extra_ldflags=[\"-L{}/lib\"]", prefix),
  };
}

// The value GN ended up with, not the value the command line asked for. An
// argument that is misspelled, overridden or shadowed still looks right in a
// copied command; it does not look right here.
[[nodiscard]] inline bool confirmArgument(const Context &context,
                                          const std::filesystem::path &out,
                                          const std::string &name,
                                          const std::string &expected) {
  const CommandResult listed =
      run({.fProgram = context.fTools.fGn,
           .fArguments = {"args", out.string(), std::format("--list={}", name),
                          "--short"},
           .fDirectory = out.parent_path().parent_path()});
  if (!listed.ok()) {
    log::error("gn cannot report {}", name);
    return false;
  }
  const std::string line = firstLine(listed.fOutput);
  const std::string wanted = std::format("{} = {}", name, expected);
  if (line != wanted) {
    log::error("gn reports `{}`, not `{}`", line, wanted);
    return false;
  }
  log::debug("gn confirms {}", wanted);
  return true;
}

[[nodiscard]] inline bool installSkia(const Context &context,
                                      const std::filesystem::path &source,
                                      const std::filesystem::path &out) {
  const std::filesystem::path prefix = context.fLayout.prefix();
  const std::filesystem::path headers = prefix / "include" / "skia";
  std::error_code code;
  CopyReport report;
  for (const std::string_view directory : {"include", "modules", "src"}) {
    const std::filesystem::path from = source / directory;
    if (std::filesystem::exists(from, code)) {
      copyTree(from, headers / directory, {}, report);
    }
  }
  log::info("{} header files", report.fCopied);

  std::size_t archives = 0;
  std::filesystem::create_directories(prefix / "lib", code);
  for (const auto &entry : std::filesystem::directory_iterator(out, code)) {
    if (entry.is_regular_file(code) && entry.path().extension() == ".a") {
      std::filesystem::copy_file(
          entry.path(), prefix / "lib" / entry.path().filename(),
          std::filesystem::copy_options::overwrite_existing, code);
      ++archives;
    }
  }
  if (archives == 0) {
    log::error("the Skia build produced no archives in {}", out.string());
    return false;
  }
  log::info("{} archives", archives);

  // The revision Skia was actually built from. A placeholder version in a
  // pkg-config file is a build that cannot say what it contains.
  const std::optional<std::string> revision = git::head(source);
  if (!revision) {
    log::error("cannot read the Skia revision");
    return false;
  }
  return build::writePackageConfig(
      context, "skia", *revision, "libpng libjpeg zlib libwebp freetype2",
      "-L${libdir} -lskia -lskcms -lEGL -lGLESv3 -landroid -llog -ldl -lm",
      "-I${includedir} -I${includedir}/skia");
}

// Skia is a package like the others -- it is asked for by name and it is not
// built when it is not -- but it is not built like the others: GN, a set of
// arguments that have to be confirmed afterwards, and an install that is
// assembled here because Skia has no install of its own.
[[nodiscard]] inline bool buildSkia(Context &context, const Package &package) {
  const std::filesystem::path source =
      context.fLayout.sourceOf(package.fSource);
  const std::filesystem::path out = source / "out" / "android-arm64";
  const std::vector<std::string> arguments = gnArguments(context);
  if (!runChecked({.fProgram = context.fTools.fGn,
                   .fArguments = {"gen", out.string(),
                                  std::format("--args={}",
                                              text::join(arguments, " "))},
                   .fDirectory = source},
                  "gn gen")) {
    return false;
  }
  if (!confirmArgument(context, out, "skia_use_vulkan", "false") ||
      !confirmArgument(context, out, "skia_use_system_zlib", "true") ||
      !confirmArgument(context, out, "skia_use_system_freetype2", "true")) {
    return false;
  }
  if (!runChecked({.fProgram = context.fTools.fNinja,
                   .fArguments = {"-C", out.string(), "-j",
                                  std::to_string(context.fOptions.fJobs),
                                  "skia"},
                   .fDirectory = source,
                   .fCapture = false},
                  "building Skia")) {
    return false;
  }
  return installSkia(context, source, out);
}

} // namespace mandk::steps
