export module mandk.steps.apk;

import std;
import mandk.layout;
import mandk.log;
import mandk.plan;
import mandk.process;
import mandk.text;
import mandk.toolchainfile;

// The text of what gets packaged. Not exported, for the same reason as in
// the check: a name exported from two modules is the same name twice.
namespace mandk::steps {

// A real APK, made of everything this tool produced.
//
// Not a demonstration: it is the only thing that answers whether the pieces
// fit together. The sysroot compiles, the stubs link, the runtimes are in
// the archives, the resource package resolves, the signer signs -- each of
// those is checked on its own elsewhere, and none of them says whether the
// result is an APK.
inline constexpr std::string_view kApkManifest =
    R"(<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
          package="org.example.mandkcheck">
  <uses-sdk android:minSdkVersion="27" android:targetSdkVersion="35" />
  <application android:label="mandk check"
               android:hasCode="false"
               android:extractNativeLibs="true">
  </application>
</manifest>
)";

inline constexpr std::string_view kApkLibrary = R"(#include <string>
#include <vector>

// Enough of the language to need the runtimes, and nothing else: this is
// carried into the APK to prove that what was compiled can be packaged.
extern "C" int mandk_apk_check() {
  std::vector<std::string> words{"a", "b", "c"};
  std::string joined;
  for (const std::string &word : words) {
    joined += word;
  }
  return static_cast<int>(joined.size());
}
)";

inline constexpr std::string_view kApkProject = R"(cmake_minimum_required(VERSION 3.24)
project(mandk-apk LANGUAGES CXX)
add_library(mandkcheck SHARED library.cc)
target_compile_features(mandkcheck PRIVATE cxx_std_20)
)";

} // namespace mandk::steps

export namespace mandk::steps {

[[nodiscard]] inline Step apk() {
  Step step;
  step.fName = "apk";
  step.fSummary = "build, package and sign an APK out of what this produced";
  step.fNeeds = {"runtimes", "toolchain-file", "framework-res", "apksigner"};
  step.fRun = [](Context &context) {
    for (const std::string &program :
         {context.fTools.fAapt2, context.fTools.fZipalign,
          context.fTools.fKeytool, context.fTools.fJava, context.fTools.fJar}) {
      if (!haveProgram(program)) {
        log::error("{} is not on PATH, and packaging an APK needs it",
                   program);
        return false;
      }
    }

    const std::filesystem::path work = context.fLayout.buildOf("apk");
    std::error_code code;
    std::filesystem::remove_all(work, code);
    std::filesystem::create_directories(work / "source", code);
    {
      std::ofstream(work / "source" / "library.cc") << kApkLibrary;
      std::ofstream(work / "source" / "CMakeLists.txt") << kApkProject;
      std::ofstream(work / "AndroidManifest.xml") << kApkManifest;
    }

    // The native library, through the toolchain file, exactly as anything
    // else would build it.
    if (!runChecked(
            {.fProgram = context.fTools.fCmake,
             .fArguments =
                 {"-S", (work / "source").string(), "-B",
                  (work / "build").string(), "-G", "Ninja",
                  std::format("-DCMAKE_TOOLCHAIN_FILE={}",
                              toolchainFilePath(context.fLayout).string()),
                  "-DCMAKE_BUILD_TYPE=Release"}},
            "configuring the APK's library") ||
        !runChecked({.fProgram = context.fTools.fCmake,
                     .fArguments = {"--build", (work / "build").string()}},
                    "building the APK's library")) {
      return false;
    }
    std::optional<std::filesystem::path> library;
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(work / "build", code)) {
      if (entry.path().extension() == ".so") {
        library = entry.path();
        break;
      }
    }
    if (!library) {
      log::error("nothing was built to put in the APK");
      return false;
    }

    // Resources and manifest, against the platform's resource package. That
    // is what -I is for, and there is no android.jar anywhere in this.
    const std::filesystem::path unsigned_ = work / "unsigned.apk";
    if (!runChecked(
            {.fProgram = context.fTools.fAapt2,
             .fArguments = {"link", "-o", unsigned_.string(), "--manifest",
                            (work / "AndroidManifest.xml").string(), "-I",
                            (context.fLayout.tools() / "framework-res.apk")
                                .string(),
                            "--min-sdk-version",
                            std::to_string(context.target().fApi),
                            "--target-sdk-version",
                            std::to_string(context.fManifest.fTargetSdk)}},
            "linking the APK")) {
      return false;
    }

    // The library goes in under the ABI it was built for, which is how
    // Android finds it.
    const std::filesystem::path staging =
        work / "payload" / "lib" / context.target().fAbi;
    std::filesystem::create_directories(staging, code);
    std::filesystem::copy_file(*library, staging / library->filename(),
                               std::filesystem::copy_options::
                                   overwrite_existing,
                               code);
    if (code) {
      log::error("cannot put {} in the APK: {}", library->string(),
                 code.message());
      return false;
    }
    if (!runChecked({.fProgram = context.fTools.fJar,
                     .fArguments = {"--update", "--file", unsigned_.string(),
                                    "-C", (work / "payload").string(), "lib"}},
                    "adding the library to the APK")) {
      return false;
    }

    const std::filesystem::path aligned = work / "aligned.apk";
    if (!runChecked({.fProgram = context.fTools.fZipalign,
                     .fArguments = {"-f", "4", unsigned_.string(),
                                    aligned.string()}},
                    "aligning the APK")) {
      return false;
    }

    // A key of its own, made here and thrown away with the rest of the
    // scratch: signing with somebody's real key to find out whether signing
    // works is not a thing to do.
    const std::filesystem::path keystore = work / "check.keystore";
    if (!runChecked(
            {.fProgram = context.fTools.fKeytool,
             .fArguments = {"-genkeypair", "-keystore", keystore.string(),
                            "-storepass", "android", "-keypass", "android",
                            "-alias", "check", "-keyalg", "RSA", "-keysize",
                            "2048", "-validity", "3650", "-dname",
                            "CN=minimal-android-ndk check"}},
            "making a key to sign with")) {
      return false;
    }

    const std::filesystem::path signed_ = context.fLayout.tools() / "check.apk";
    std::filesystem::create_directories(signed_.parent_path(), code);
    const std::filesystem::path signer =
        context.fLayout.tools() / "apksigner.jar";
    if (!runChecked({.fProgram = context.fTools.fJava,
                     .fArguments = {"-jar", signer.string(), "sign", "--ks",
                                    keystore.string(), "--ks-pass",
                                    "pass:android", "--key-pass",
                                    "pass:android", "--ks-key-alias", "check",
                                    "--out", signed_.string(),
                                    aligned.string()}},
                    "signing the APK")) {
      return false;
    }

    // Asked of the signer rather than assumed: a file that was written is
    // not the same as a file Android will accept.
    const CommandResult verified =
        run({.fProgram = context.fTools.fJava,
             .fArguments = {"-jar", signer.string(), "verify", "--verbose",
                            signed_.string()}});
    if (!verified.ok()) {
      log::error("the APK was signed and does not verify:");
      for (const auto &line : text::split(verified.fOutput, '\n')) {
        if (!text::trim(line).empty()) {
          log::error("  | {}", line);
        }
      }
      return false;
    }
    const std::uintmax_t size = std::filesystem::file_size(signed_, code);
    log::info("{} ({} KiB), signed and verified", signed_.string(),
              size / 1024);
    log::note("install it with: adb install -r {}", signed_.string());
    return true;
  };
  return step;
}

} // namespace mandk::steps
