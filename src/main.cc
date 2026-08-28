import std;
import mandk.journal;
import mandk.layout;
import mandk.log;
import mandk.manifest;
import mandk.manifest.io;
import mandk.pin;
import mandk.plan;
import mandk.process;
import mandk.steps.apk;
import mandk.steps.apksigner;
import mandk.steps.check;
import mandk.steps.crt;
import mandk.steps.framework;
import mandk.steps.hashes;
import mandk.steps.headers;
import mandk.steps.planned;
import mandk.steps.runtimes;
import mandk.steps.sources;
import mandk.steps.stubs;
import mandk.steps.thirdparty;
import mandk.toolchainfile;

namespace {

using namespace mandk;

[[nodiscard]] Plan wholePlan() {
  Plan plan;
  plan.add(steps::sources());
  plan.add(steps::ndkCompat());
  plan.add(steps::headers());
  plan.add(steps::apiStubs());
  plan.add(steps::crt());
  plan.add(toolchainFile());
  plan.add(steps::compilerRt());
  plan.add(steps::runtimes());
  plan.add(steps::check());
  plan.add(steps::apksigner());
  plan.add(steps::frameworkRes());
  plan.add(steps::apk());
  plan.add(steps::thirdParty());
  for (Step &step : steps::remaining()) {
    plan.add(std::move(step));
  }
  plan.add(steps::hashes());
  return plan;
}

void usage() {
  std::cout << R"(minimal-android-ndk -- build an Android toolchain out of Android's sources

  minimal-android-ndk pin [--platform TAG]   resolve every source to a commit
  minimal-android-ndk plan                   show the steps, in the order they run
  minimal-android-ndk build [STEP...]        run the steps (default: every ready one)
  minimal-android-ndk hashes                 record what was built and from what
  minimal-android-ndk list                   the libraries that can be built
  minimal-android-ndk env                    print the environment and the CMake flags

Libraries
  --with NAME       build this library, and whatever it needs
  --all             build every library in the manifest
                    (`list` shows them; nothing is built unless it is asked
                    for, and no repository is fetched for a library that is
                    not being built)

Options
  --root DIR        where the toolchain is put      (default $ANDROID_MINIMAL,
                    otherwise ~/android-minimal)
  --build-root DIR  where things are built on the way (default ./ndk, or
                    $MANDK_BUILD)
  --manifest FILE   the pinned source list         (default $MANDK_MANIFEST,
                    otherwise ./manifest/sources.json)
  --api N           override the API level in the manifest
  --clang NAME      host compiler                  (default clang)
  --python NAME     host python                    (default python3)
  --readelf NAME    stub verifier                  (default llvm-readelf)
  --llvm-bin DIR    where llvm-ar and llvm-ranlib are, when not on PATH
  --jobs N, -j N    parallel jobs for the dependency builds
  --reuse           trust stamps written by an older build of this program.
                    Every step is normally redone when the program changes,
                    because a step that finished was finished by a different
                    program; this says you know what you changed.
  --force           run steps even when their stamp says they are done
  --dry-run         say what would run, run nothing
  --keep-going      do not stop at the first failed step
  --verbose         show every command
)";
}

struct Invocation {
  std::string fCommand;
  std::vector<std::string> fRest;
  std::vector<std::string> fWith;
  bool fAll = false;
  std::filesystem::path fRoot;
  std::filesystem::path fBuild;
  std::filesystem::path fManifest;
  std::string fPlatform;
  std::optional<int> fApi;
  Options fOptions;
  Tools fTools;
};

[[nodiscard]] std::filesystem::path defaultRoot() {
  if (const char *const set = std::getenv("ANDROID_MINIMAL"); set != nullptr) {
    return set;
  }
  if (const char *const home = std::getenv("HOME"); home != nullptr) {
    return std::filesystem::path(home) / "android-minimal";
  }
  return std::filesystem::current_path() / "android-minimal";
}

[[nodiscard]] std::optional<Invocation> parse(std::span<const std::string> args) {
  Invocation invocation;
  invocation.fRoot = defaultRoot();
  // Beside where the tool was run rather than inside what it produces: a
  // scratch tree in the middle of a toolchain is hard to tell from the
  // toolchain.
  // As many as the machine has. One was a placeholder that nobody chose and
  // everybody paid for.
  invocation.fOptions.fJobs =
      static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  invocation.fBuild = std::filesystem::current_path() / "ndk";
  if (const char *const set = std::getenv("MANDK_BUILD"); set != nullptr) {
    invocation.fBuild = set;
  }
  invocation.fManifest = "manifest/sources.json";
  if (const char *const set = std::getenv("MANDK_MANIFEST"); set != nullptr) {
    invocation.fManifest = set;
  }
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string &argument = args[i];
    const auto value = [&](std::string_view name) -> std::optional<std::string> {
      if (i + 1 >= args.size()) {
        log::error("{} needs a value", name);
        return std::nullopt;
      }
      return args[++i];
    };
    if (argument == "--help" || argument == "-h") {
      usage();
      return std::nullopt;
    } else if (argument == "--root" || argument == "-C") {
      const auto given = value(argument);
      if (!given) return std::nullopt;
      invocation.fRoot = *given;
    } else if (argument == "--build-root") {
      const auto given = value(argument);
      if (!given) return std::nullopt;
      invocation.fBuild = *given;
    } else if (argument == "--manifest") {
      const auto given = value(argument);
      if (!given) return std::nullopt;
      invocation.fManifest = *given;
    } else if (argument == "--platform") {
      const auto given = value(argument);
      if (!given) return std::nullopt;
      invocation.fPlatform = *given;
    } else if (argument == "--api") {
      const auto given = value(argument);
      if (!given) return std::nullopt;
      invocation.fApi = std::stoi(*given);
    } else if (argument == "--clang") {
      const auto given = value(argument);
      if (!given) return std::nullopt;
      invocation.fTools.fClang = *given;
    } else if (argument == "--python") {
      const auto given = value(argument);
      if (!given) return std::nullopt;
      invocation.fTools.fPython = *given;
    } else if (argument == "--readelf") {
      const auto given = value(argument);
      if (!given) return std::nullopt;
      invocation.fTools.fReadelf = *given;
    } else if (argument == "--llvm-bin") {
      const auto given = value(argument);
      if (!given) return std::nullopt;
      invocation.fTools.fLlvmBin = *given;
    } else if (argument == "--jobs" || argument == "-j") {
      const auto given = value(argument);
      if (!given) return std::nullopt;
      invocation.fOptions.fJobs = std::stoi(*given);
    } else if (argument == "--with") {
      const auto given = value(argument);
      if (!given) return std::nullopt;
      invocation.fWith.push_back(*given);
    } else if (argument == "--all") {
      invocation.fAll = true;
    } else if (argument == "--reuse") {
      invocation.fOptions.fReuse = true;
    } else if (argument == "--force") {
      invocation.fOptions.fForce = true;
    } else if (argument == "--dry-run") {
      invocation.fOptions.fDryRun = true;
    } else if (argument == "--keep-going") {
      invocation.fOptions.fKeepGoing = true;
    } else if (argument == "--verbose" || argument == "-v") {
      log::sink().setVerbose(true);
    } else if (argument.starts_with("-")) {
      log::error("unknown option {}", argument);
      return std::nullopt;
    } else if (invocation.fCommand.empty()) {
      invocation.fCommand = argument;
    } else {
      invocation.fRest.push_back(argument);
    }
  }
  if (invocation.fCommand.empty()) {
    usage();
    return std::nullopt;
  }
  return invocation;
}

// What is going to be built: the libraries named, plus everything they need.
// Nothing is built by default -- a toolchain is what this produces, and which
// libraries belong on top of it is not something it can guess.
[[nodiscard]] std::optional<std::set<std::string>>
choosePackages(const Manifest &manifest, std::span<const std::string> with,
               bool all) {
  std::vector<std::string> requested(with.begin(), with.end());
  if (all) {
    for (const Package &package : manifest.fPackages) {
      requested.push_back(package.fName);
    }
  }
  return manifest.closure(requested);
}

void printPackages(const Context &context) {
  std::cout << std::format("  {:<5} {:<16} {}\n", "", "library", "needs");
  for (const Package &package : context.fManifest.fPackages) {
    std::string needs;
    for (const std::string &need : package.fNeeds) {
      needs += needs.empty() ? need : ", " + need;
    }
    std::cout << std::format("  {:<5} {:<16} {}\n",
                             context.wants(package.fName) ? "yes" : "",
                             package.fName, needs);
    if (!package.fNote.empty()) {
      std::cout << std::format("        {}\n", package.fNote);
    }
  }
}

void printPlan(const Plan &plan, const Context &context) {
  const std::vector<std::string> goals = plan.allNames();
  const std::optional<std::vector<std::string>> ordered = plan.order(goals);
  if (!ordered) {
    return;
  }
  for (const std::string &name : *ordered) {
    const Step *const step = plan.find(name);
    const std::optional<std::string> key =
        step->fKey ? step->fKey(context) : std::nullopt;
    const std::string_view state =
        !step->fReady                          ? "planned"
        : context.fJournal.upToDate(name, key) ? "done"
        : !key                                 ? "always"
                                               : "todo";
    std::cout << std::format("  {:<7} {:<16} {}\n", state, name,
                             step->fSummary);
  }
}

void printEnvironment(const Context &context) {
  const Layout &layout = context.fLayout;
  const Target &target = context.target();
  std::cout << std::format(
      "export ANDROID_MINIMAL={}\n"
      "export ANDROID_API={}\n"
      "export ANDROID_TRIPLE={}\n"
      "export ANDROID_LIBDIR={}\n"
      "\n"
      "cmake -S standalone -B build/android-minimal -G Ninja \\\n"
      "  -DCMAKE_TOOLCHAIN_FILE=\"$PWD/cmake/toolchains/android-minimal.cmake\" \\\n"
      "  -DCMAKE_BUILD_TYPE=Release \\\n"
      "  -DCMAKE_PREFIX_PATH=\"{}\" \\\n"
      "  -DOSU_ANDROID_FRAMEWORK_RES_APK=\"{}\" \\\n"
      "  -DOSU_ANDROID_APKSIGNER_JAR=\"$HOME/.local/lib/apksigner/apksigner.jar\" \\\n"
      "  -DOSU_ANDROID_SYSTEM_FILE_PICKER=OFF\n",
      layout.root().string(), target.fApi, target.fTriple,
      layout.sysrootLib(target).string(), layout.prefix().string(),
      (layout.buildOf("framework") / "framework-res.apk").string());
}

} // namespace

int main(int argc, char **argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  std::optional<Invocation> invocation = parse(args);
  if (!invocation) {
    return 1;
  }

  std::optional<Manifest> manifest = loadManifest(invocation->fManifest);
  if (!manifest) {
    return 1;
  }
  if (invocation->fApi) {
    manifest->fTarget.fApi = *invocation->fApi;
  }

  // Every tool becomes a path, and every path becomes absolute.
  //
  // CMake reads CMAKE_AR as a path, so a bare name is a file in whatever
  // directory the build happened to start in -- which is how "ar" became
  // /home/user/minimal-android-ndk.cpp/ar and was not found. And a relative
  // root is worse than useless once anything changes directory, which CMake
  // does for every compiler probe.
  Tools tools = invocation->fTools;
  std::string llvmBin = tools.fLlvmBin;
  if (llvmBin.empty()) {
    // Where the LLVM tools are is not always on PATH -- /usr/lib/llvm/22/bin
    // is not -- but the compiler knows where it lives, and they live beside
    // it.
    const CommandResult resource =
        run({.fProgram = tools.fClang, .fArguments = {"-print-resource-dir"}});
    if (resource.ok()) {
      std::filesystem::path where(firstLine(resource.fOutput));
      // .../lib/clang/<version> -> .../bin
      where = where.parent_path().parent_path().parent_path() / "bin";
      std::error_code code;
      if (std::filesystem::is_directory(where, code)) {
        llvmBin = where.string();
        log::debug("the compiler's own tools are in {}", llvmBin);
      }
    }
  }

  const auto settle = [&llvmBin](std::string &configured,
                                 std::initializer_list<std::string_view>
                                     alternatives) {
    std::vector<std::string> candidates{configured};
    if (!llvmBin.empty()) {
      candidates.push_back((std::filesystem::path(llvmBin) / configured)
                               .string());
    }
    for (const std::string_view other : alternatives) {
      if (!llvmBin.empty()) {
        candidates.push_back((std::filesystem::path(llvmBin) / other).string());
      }
      candidates.emplace_back(other);
    }
    for (const std::string &candidate : candidates) {
      const std::filesystem::path found = findProgram(candidate);
      if (found.empty()) {
        continue;
      }
      std::error_code code;
      const std::string resolved =
          std::filesystem::absolute(found, code).string();
      if (resolved != configured) {
        log::info("{} is {}", configured, resolved);
      }
      configured = resolved;
      return;
    }
    log::warn("{} was not found anywhere; leaving it as it is", configured);
  };
  settle(tools.fClang, {});
  settle(tools.fClangxx, {});
  settle(tools.fReadelf, {"readelf"});
  settle(tools.fAr, {"ar"});
  settle(tools.fRanlib, {"ranlib"});

  // Absolute, because a compiler probe runs in a directory of CMake's
  // choosing and a relative sysroot means nothing there.
  std::error_code pathCode;
  const Layout layout(
      std::filesystem::absolute(invocation->fRoot, pathCode),
      std::filesystem::absolute(invocation->fBuild, pathCode));
  std::error_code code;
  std::filesystem::create_directories(layout.logs(), code);
  log::sink().setTranscript(layout.logs() / "minimal-android-ndk.log");

  const std::optional<std::set<std::string>> packages =
      choosePackages(*manifest, invocation->fWith, invocation->fAll);
  if (!packages) {
    return 1;
  }

  // The program itself, as size and time. A step that finished before this
  // build of the tool existed was finished by a different program.
  std::string self;
  {
    std::error_code code;
    std::filesystem::path binary =
        std::filesystem::read_symlink("/proc/self/exe", code);
    if (code || binary.empty()) {
      binary = argv[0];
    }
    const auto size = std::filesystem::file_size(binary, code);
    const auto when = std::filesystem::last_write_time(binary, code);
    self = std::format("{}|{}|{}", binary.string(), size,
                       when.time_since_epoch().count());
  }

  Context context{.fLayout = layout,
                  .fManifest = *manifest,
                  .fJournal = Journal(layout.stamps()),
                  .fOptions = invocation->fOptions,
                  .fTools = tools,
                  .fPackages = *packages,
                  .fSelf = self};

  const Plan plan = wholePlan();
  const std::string &command = invocation->fCommand;

  if (command == "plan") {
    std::cout << std::format(
        "toolchain {}\nbuilt in  {}\nmanifest  {}\ntarget    {} api {}\n\n",
        layout.root().string(), layout.build().string(),
        invocation->fManifest.string(), context.target().fTriple,
        context.target().fApi);
    printPlan(plan, context);
    if (!context.fPackages.empty()) {
      std::cout << std::format("\n{} libraries asked for\n",
                               context.fPackages.size());
    }
    return 0;
  }
  if (command == "list") {
    printPackages(context);
    return 0;
  }
  if (command == "env") {
    printEnvironment(context);
    return 0;
  }
  if (command == "pin") {
    if (!haveProgram(std::string("git"))) {
      log::error("git is not on PATH");
      return 1;
    }
    return pin(*manifest, layout, invocation->fPlatform) ? 0 : 1;
  }
  if (command == "build" || command == "hashes") {
    std::vector<std::string> goals = invocation->fRest;
    if (command == "hashes") {
      goals = {"hashes"};
    } else if (goals.empty()) {
      for (const Step &step : plan.steps()) {
        if (step.fReady && step.fName != "hashes") {
          goals.push_back(step.fName);
        }
      }
    }
    for (const std::string &program :
         {tools.fClang, tools.fPython, std::string("git")}) {
      if (!haveProgram(program)) {
        log::error("{} is not on PATH", program);
        return 1;
      }
    }
    return plan.run(context, goals) ? 0 : 1;
  }

  log::error("unknown command {}", command);
  usage();
  return 1;
}
