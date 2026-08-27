export module mandk.steps.crt;

import std;
import mandk.layout;
import mandk.log;
import mandk.plan;
import mandk.process;
import mandk.search;
import mandk.sha256;

namespace mandk::steps {

// What the linker asks for when the compiler was configured with a stack
// protector on by default. The archive holds one function: distributions put
// __stack_chk_fail_local in libssp_nonshared.a rather than in the C library
// because a hidden, non-PLT copy is what position-independent code on some
// architectures needs, and the compiler's spec file then names the archive
// on every link. Android's C library has __stack_chk_fail; it has never had
// the local wrapper, because the NDK's compiler does not ask for one.
constexpr std::string_view kStackGuardSource = R"(extern void __stack_chk_fail(void);

__attribute__((visibility("hidden"))) void __stack_chk_fail_local(void) {
  __stack_chk_fail();
}
)";

} // namespace mandk::steps

export namespace mandk::steps {

// The startup objects.
//
// A shared library on Android is not just its own code. The linker puts
// crtbegin_so.o at the front of it and crtend_so.o at the end, and they are
// where __dso_handle lives, where the destructor that calls __cxa_finalize
// on unload lives, and where the notes are that tell the platform's dynamic
// linker which API level this was built for and whether its segments may be
// padded for a sixteen kilobyte page. Without them a link fails on the two
// file names and says nothing about what they are.
//
// They are three lines of C and two of assembly, in bionic, next to the
// library they belong to. The official NDK ships them prebuilt; there is no
// reason they cannot be built.
//
// The shared-library pair is what an APK's native code needs. The
// executable pair is built as well, and for one reason: a sysroot that
// cannot link a program cannot answer a question about a function either.
// Every CMake check of the form "does this exist" links one, and a
// toolchain that makes those checks stop at an archive answers yes to all
// of them.
[[nodiscard]] inline Step crt() {
  Step step;
  step.fName = "crt";
  step.fSummary = "build bionic's startup objects";
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
    hash.update(std::string("crt"));
    return hash.hex();
  };
  step.fRun = [](Context &context) {
    const Target &target = context.target();
    const std::filesystem::path bionic = context.fLayout.sourceOf("bionic");
    const std::optional<std::filesystem::path> common =
        search::directory(bionic, "libc/arch-common/bionic");
    if (!common) {
      log::error("the bionic checkout has no libc/arch-common/bionic; that is "
                 "where the startup objects are");
      return false;
    }

    std::error_code code;
    const std::filesystem::path work = context.fLayout.buildOf("crt");
    std::filesystem::remove_all(work, code);
    std::filesystem::create_directories(work, code);
    const std::filesystem::path lib = context.fLayout.sysrootLib(target);
    std::filesystem::create_directories(lib, code);

    // -I <bionic>/libc, because the assembly reaches for private/bionic_asm.h
    // and its architecture half. PLATFORM_SDK_VERSION is what crtbrand.S
    // writes into the note: the platform reads it back as the API level the
    // library was built against, so it is this build's API level and not the
    // one the source tree happens to be from.
    const std::vector<std::string> flags{
        std::format("--target={}", target.clangTarget()),
        std::format("--sysroot={}", context.fLayout.sysroot().string()),
        std::format("-I{}", (bionic / "libc").string()),
        std::format("-DPLATFORM_SDK_VERSION={}", target.fApi),
        "-fPIC",
        "-O2",
        "-c",
    };

    const auto compile = [&](std::string_view name,
                             const std::filesystem::path &object) {
      const std::filesystem::path source = *common / name;
      if (!std::filesystem::exists(source, code)) {
        log::error("bionic has no {}", source.string());
        return false;
      }
      std::vector<std::string> arguments = flags;
      arguments.push_back(source.string());
      arguments.push_back("-o");
      arguments.push_back(object.string());
      return runChecked({.fProgram = context.fTools.fClang,
                         .fArguments = std::move(arguments)},
                        std::format("compiling {}", name));
    };

    // crtbegin_so is three objects in one: the C part, and the two notes.
    // They are combined into a single relocatable object because that is
    // what the linker expects to find under one file name.
    const std::filesystem::path beginPart = work / "crtbegin_so.c.o";
    const std::filesystem::path brand = work / "crtbrand.o";
    const std::filesystem::path pad = work / "crt_pad_segment.o";
    if (!compile("crtbegin_so.c", beginPart) || !compile("crtbrand.S", brand) ||
        !compile("crt_pad_segment.S", pad)) {
      return false;
    }
    if (!runChecked(
            {.fProgram = context.fTools.fClang,
             .fArguments = {std::format("--target={}", target.clangTarget()),
                            "-fuse-ld=lld", "-nostdlib", "-r", "-o",
                            (lib / "crtbegin_so.o").string(), beginPart.string(),
                            brand.string(), pad.string()}},
            "combining the three parts of crtbegin_so.o")) {
      return false;
    }
    if (!compile("crtend_so.S", lib / "crtend_so.o")) {
      return false;
    }

    // And the executable pair, which is what makes a compiler probe able to
    // link.
    //
    // Without it every CMake check that asks "does this function exist" by
    // linking has to stop at an archive, and an archive keeps its undefined
    // symbols: the answer is yes to everything. libzip asked for memcpy_s
    // that way and got it, and bionic has no Annex K at all.
    //
    // crtbegin.c is the same shape as crtbegin_so.c -- a C part and the two
    // notes -- and reaches into bionic's own headers by relative path,
    // which is why it is compiled from the checkout rather than against the
    // sysroot alone. If any of this does not build, the shared pair above
    // is still there and the toolchain file says probes stop at an archive.
    const std::filesystem::path dynamicPart = work / "crtbegin.c.o";
    if (compile("crtbegin.c", dynamicPart) &&
        runChecked(
            {.fProgram = context.fTools.fClang,
             .fArguments = {std::format("--target={}", target.clangTarget()),
                            "-fuse-ld=lld", "-nostdlib", "-r", "-o",
                            (lib / "crtbegin_dynamic.o").string(),
                            dynamicPart.string(), brand.string(), pad.string()}},
            "combining the three parts of crtbegin_dynamic.o") &&
        compile("crtend.S", lib / "crtend_android.o")) {
      log::info("{}", (lib / "crtbegin_dynamic.o").string());
      log::info("{}", (lib / "crtend_android.o").string());
    } else {
      std::filesystem::remove(lib / "crtbegin_dynamic.o", code);
      std::filesystem::remove(lib / "crtend_android.o", code);
      log::warn("bionic's executable startup objects did not build here, so "
                "nothing in this sysroot can link a program: every compiler "
                "probe that asks by linking will answer yes");
    }

    const std::filesystem::path guard = work / "__stack_chk_fail_local.c";
    std::ofstream(guard) << kStackGuardSource;
    const std::filesystem::path guardObject = work / "__stack_chk_fail_local.o";
    {
      std::vector<std::string> arguments = flags;
      arguments.push_back(guard.string());
      arguments.push_back("-o");
      arguments.push_back(guardObject.string());
      if (!runChecked({.fProgram = context.fTools.fClang,
                       .fArguments = std::move(arguments)},
                      "compiling the stack guard wrapper")) {
        return false;
      }
    }
    const std::filesystem::path archive = lib / "libssp_nonshared.a";
    std::filesystem::remove(archive, code);
    if (!runChecked({.fProgram = context.fTools.fAr,
                     .fArguments = {"rcs", archive.string(),
                                    guardObject.string()}},
                    "making libssp_nonshared.a")) {
      return false;
    }

    for (const std::string_view name :
         {"crtbegin_so.o", "crtend_so.o", "libssp_nonshared.a"}) {
      const std::filesystem::path made = lib / name;
      if (!std::filesystem::exists(made, code)) {
        log::error("{} is not there after the step that makes it",
                   made.string());
        return false;
      }
      log::info("{}", made.string());
    }
    return true;
  };
  return step;
}

} // namespace mandk::steps
