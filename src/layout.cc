export module mandk.layout;

import std;

export namespace mandk {

// One ABI and one API level. The two spellings of the architecture are both
// needed: ndkstubgen and the AOSP map files say arm64, while the APK and the
// library directory inside an APK say arm64-v8a.
struct Target {
  std::string fTriple = "aarch64-linux-android";
  std::string fArch = "arm64";
  std::string fAbi = "arm64-v8a";
  int fApi = 27;
  // The triple the sysroot's library directory and the multiarch names
  // use, where it is not the one Clang is given: 32-bit ARM is
  // armv7a-linux-androideabi to Clang -- arm-linux-androideabi would be
  // ARMv5 -- and arm-linux-androideabi to the directories Clang searches.
  // Empty: the same as fTriple.
  std::string fSysrootTriple;
  // What every compile for this target is given besides --target: what
  // Clang does not assume for it. 32-bit ARM is soft float with no FPU to
  // Clang 23 unless told -- the NDK says -mfpu=neon -mfloat-abi=softfp
  // -mthumb itself -- and everything built for it would emulate its
  // floating point. Nothing, for arm64.
  std::vector<std::string> fFlags;

  // The flags as one argument for a CMake *_FLAGS variable.
  [[nodiscard]] std::string flagText() const {
    return fFlags | std::views::join_with(' ') | std::ranges::to<std::string>();
  }

  [[nodiscard]] const std::string &sysrootTriple() const {
    return fSysrootTriple.empty() ? fTriple : fSysrootTriple;
  }
  // The processor, as CMake names it: the sysroot triple's first word --
  // aarch64, arm.
  [[nodiscard]] std::string processor() const {
    const std::string &triple = this->sysrootTriple();
    return triple.substr(0, triple.find('-'));
  }

  // What Clang is given: the API level is part of the target, not a flag
  // beside it.
  [[nodiscard]] std::string clangTarget() const {
    return std::format("{}{}", fTriple, fApi);
  }
};

// The tree the build produces. The names match the ones the osu-cpp Android
// notes use, so a tree built by hand and a tree built by this tool are the
// same tree.
class Layout {
public:
  // Two roots, because two different things live in them. One holds what is
  // produced and kept -- the sysroot, the stubs, the runtimes, the prefix --
  // and the other holds what is only needed while producing it. Mixing them
  // means a scratch directory sitting inside the toolchain it was used to
  // make, which is confusing to look at and worse to delete.
  Layout(std::filesystem::path root, std::filesystem::path build)
      : fRoot(std::move(root)), fBuild(std::move(build)) {}

  [[nodiscard]] const std::filesystem::path &root() const { return fRoot; }
  [[nodiscard]] std::filesystem::path sources() const { return fRoot / "src"; }
  [[nodiscard]] std::filesystem::path sourceOf(std::string_view name) const {
    return this->sources() / name;
  }
  [[nodiscard]] const std::filesystem::path &build() const { return fBuild; }
  [[nodiscard]] std::filesystem::path buildOf(std::string_view name) const {
    return fBuild / name;
  }
  [[nodiscard]] std::filesystem::path sysroot() const { return fRoot / "sysroot"; }
  [[nodiscard]] std::filesystem::path sysrootInclude() const {
    return this->sysroot() / "usr" / "include";
  }
  // The link stubs live where Clang looks for a versioned Android sysroot,
  // which is usr/lib/<triple>/<api>.
  [[nodiscard]] std::filesystem::path sysrootLib(const Target &target) const {
    return this->sysroot() / "usr" / "lib" / target.sysrootTriple() /
           std::to_string(target.fApi);
  }
  [[nodiscard]] std::filesystem::path prefix() const { return fRoot / "prefix"; }
  [[nodiscard]] std::filesystem::path clangResource() const {
    return fRoot / "clang-resource";
  }
  [[nodiscard]] std::filesystem::path runtimeInstall() const {
    return fRoot / "runtime-install";
  }
  // What is built here that is not a library and not a header: the signer,
  // and whatever else the making of an APK needs that a distribution does
  // not have.
  [[nodiscard]] std::filesystem::path tools() const { return fRoot / "tools"; }
  [[nodiscard]] std::filesystem::path ndkCompat() const {
    return fRoot / "ndk-compat";
  }
  [[nodiscard]] std::filesystem::path stamps() const {
    return this->build() / "stamps";
  }
  [[nodiscard]] std::filesystem::path logs() const { return fBuild / "logs"; }

private:
  std::filesystem::path fRoot;
  std::filesystem::path fBuild;
};

} // namespace mandk
