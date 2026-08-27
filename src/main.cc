import std;
import mandk.journal;
import mandk.layout;
import mandk.log;
import mandk.manifest;
import mandk.pin;
import mandk.plan;
import mandk.process;
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
  plan.add(toolchainFile());
  plan.add(steps::compilerRt());
  plan.add(steps::runtimes());
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
  --root DIR        where the toolchain is put      (default $ANDROID_FREE,
                    otherwise ~/android-free)
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
  if (const char *const set = std::getenv("ANDROID_FREE"); set != nullptr) {
    return set;
  }
  if (const char *const home = std::getenv("HOME"); home != nullptr) {
    return std::filesystem::path(home) / "android-free";
  }
  return std::filesystem::current_path() / "android-free";
}

[[nodiscard]] std::optional<Invocation> parse(std::span<const std::string> args) {
  Invocation invocation;
  invocation.fRoot = defaultRoot();
  // Beside where the tool was run rather than inside what it produces: a
  // scratch tree in the middle of a toolchain is hard to tell from the
  // toolchain.
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
      "export ANDROID_FREE={}\n"
      "export ANDROID_API={}\n"
      "export ANDROID_TRIPLE={}\n"
      "export ANDROID_LIBDIR={}\n"
      "\n"
      "cmake -S standalone -B build/android-free -G Ninja \\\n"
      "  -DCMAKE_TOOLCHAIN_FILE=\"$PWD/cmake/toolchains/android-free.cmake\" \\\n"
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
  const std::optional<Invocation> invocation = parse(args);
  if (!invocation) {
    return 1;
  }

  std::optional<Manifest> manifest = Manifest::load(invocation->fManifest);
  if (!manifest) {
    return 1;
  }
  if (invocation->fApi) {
    manifest->fTarget.fApi = *invocation->fApi;
  }

  // A tool that is called by one name on one distribution and another
  // elsewhere is worth settling before anything uses it, so that a missing
  // one is reported as missing rather than as a file that could not be read.
  Tools tools = invocation->fTools;
  const auto settle = [](std::string &configured,
                         std::initializer_list<std::string_view> alternatives) {
    if (haveProgram(configured)) {
      return;
    }
    for (const std::string_view other : alternatives) {
      if (haveProgram(other)) {
        log::info("{} is not on PATH; using {}", configured, other);
        configured = std::string(other);
        return;
      }
    }
  };
  settle(tools.fReadelf, {"llvm-readelf", "readelf"});
  settle(tools.fAr, {"llvm-ar", "ar"});
  settle(tools.fRanlib, {"llvm-ranlib", "ranlib"});

  const Layout layout(invocation->fRoot, invocation->fBuild);
  std::error_code code;
  std::filesystem::create_directories(layout.logs(), code);
  log::sink().setTranscript(layout.logs() / "minimal-android-ndk.log");

  const std::optional<std::set<std::string>> packages =
      choosePackages(*manifest, invocation->fWith, invocation->fAll);
  if (!packages) {
    return 1;
  }

  Context context{.fLayout = layout,
                  .fManifest = *manifest,
                  .fJournal = Journal(layout.stamps()),
                  .fOptions = invocation->fOptions,
                  .fTools = tools,
                  .fPackages = *packages};

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
