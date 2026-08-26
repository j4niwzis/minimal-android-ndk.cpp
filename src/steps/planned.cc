export module mandk.steps.planned;

import std;
import mandk.plan;

export namespace mandk::steps {

// The rest of the toolchain, declared but not yet built by this tool. They
// are here so that `minimal-android-ndk plan` shows the whole road and so that the order
// they have to run in is written down once, in the place that will run them.
[[nodiscard]] inline Step planned(std::string name, std::string summary,
                                  std::vector<std::string> needs) {
  Step step;
  step.fName = std::move(name);
  step.fSummary = std::move(summary);
  step.fNeeds = std::move(needs);
  step.fReady = false;
  return step;
}

[[nodiscard]] inline std::vector<Step> remaining() {
  return {
      planned("compiler-rt",
              "build the AArch64 Android builtins, which is where libc++ "
              "finds __emutls_get_address",
              {"sysroot-headers", "api-stubs"}),
      planned("runtimes",
              "build libc++, libc++abi and libunwind, and install the module "
              "sources and libc++.modules.json with them",
              {"compiler-rt"}),
      planned("third-party",
              "build the static dependency graph: zlib, libpng, libjpeg-turbo, "
              "libwebp, freetype, libzip, libsndfile, ogg, vorbis, FLAC, opus, "
              "mpg123, liblzma, Oboe and OpenAL Soft",
              {"runtimes"}),
      planned("skia",
              "build Ganesh for GLES with Vulkan off and install skia.pc with "
              "the revision it was built from",
              {"third-party"}),
      planned("framework-res",
              "link the platform resource package that aapt2 is given with -I",
              {"sources"}),
      planned("apksigner",
              "build the apksig jar without the KMS providers",
              {"sources"}),
  };
}

} // namespace mandk::steps
