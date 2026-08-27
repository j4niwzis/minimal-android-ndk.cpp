export module mandk.steps.check;

import std;
import mandk.layout;
import mandk.log;
import mandk.plan;
import mandk.process;
import mandk.text;
import mandk.toolchainfile;

export namespace mandk::steps {

// Whether the toolchain works, asked of something built with it rather than
// of the files it is made of.
//
// A shared library and not a program: this sysroot has no startup objects,
// on purpose, and an Android application is a shared library the platform
// loads anyway.
inline constexpr std::string_view kProgram = R"(#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// thread_local is here because it is what needs __emutls_get_address, which
// is what compiler-rt is built for. Without the builtins this is the symbol
// that goes missing.
thread_local int visits = 0;

// A virtual destructor asks for a typeinfo, which is libc++abi.
struct Shape {
  virtual ~Shape() = default;
  virtual int sides() const = 0;
};
struct Square : Shape {
  int sides() const override { return 4; }
};

// throw and catch across a function boundary, which is libunwind.
extern "C" int mandk_check(int count) {
  ++visits;
  std::vector<std::unique_ptr<Shape>> shapes;
  for (int index = 0; index < count; ++index) {
    shapes.push_back(std::make_unique<Square>());
  }
  int total = 0;
  try {
    for (const auto &shape : shapes) {
      if (shape->sides() != 4) {
        throw std::runtime_error(std::string("not a square"));
      }
      total += shape->sides();
    }
  } catch (const std::exception &error) {
    return -static_cast<int>(std::string(error.what()).size());
  }
  return total + visits;
}
)";

inline constexpr std::string_view kProject = R"(cmake_minimum_required(VERSION 3.24)
project(mandk-check LANGUAGES CXX)
add_library(mandk-check SHARED check.cc)
target_compile_features(mandk-check PRIVATE cxx_std_20)
)";

// The names a shared object says it needs, out of the dynamic section.
[[nodiscard]] inline std::vector<std::string>
neededLibraries(const Context &context, const std::filesystem::path &object) {
  const CommandResult result =
      run({.fProgram = context.fTools.fReadelf,
           .fArguments = {"--dynamic", "--wide", object.string()}});
  std::vector<std::string> names;
  if (!result.ok()) {
    return names;
  }
  for (const auto &line : text::split(result.fOutput, '\n')) {
    if (line.find("(NEEDED)") == std::string::npos) {
      continue;
    }
    const std::size_t open = line.find('[');
    const std::size_t close = line.find(']', open);
    if (open != std::string::npos && close != std::string::npos) {
      names.push_back(line.substr(open + 1, close - open - 1));
    }
  }
  return names;
}

// Every symbol a shared object still expects somebody else to have, and
// every symbol it offers.
inline void dynamicSymbols(const Context &context,
                           const std::filesystem::path &object,
                           std::set<std::string> &undefined,
                           std::set<std::string> &defined) {
  const CommandResult result =
      run({.fProgram = context.fTools.fReadelf,
           .fArguments = {"--dyn-syms", "--wide", object.string()}});
  if (!result.ok()) {
    return;
  }
  for (const auto &line : text::split(result.fOutput, '\n')) {
    const std::vector<std::string> fields = [&line] {
      std::vector<std::string> parts;
      for (const auto &piece : text::split(line, ' ')) {
        if (!text::trim(piece).empty()) {
          parts.emplace_back(text::trim(piece));
        }
      }
      return parts;
    }();
    // Num: Value Size Type Bind Vis Ndx Name
    if (fields.size() < 8 || !fields[0].ends_with(":")) {
      continue;
    }
    std::string name = fields[7];
    // A versioned name is the same symbol.
    const std::size_t at = name.find('@');
    if (at != std::string::npos) {
      name = name.substr(0, at);
    }
    if (name.empty()) {
      continue;
    }
    if (fields[6] == "UND") {
      undefined.insert(name);
    } else {
      defined.insert(name);
    }
  }
}

[[nodiscard]] inline Step check() {
  Step step;
  step.fName = "check";
  step.fSummary = "build something with the toolchain and read what came out";
  step.fNeeds = {"runtimes", "toolchain-file"};
  step.fRun = [](Context &context) {
    const std::filesystem::path work = context.fLayout.buildOf("check");
    std::error_code code;
    std::filesystem::remove_all(work, code);
    std::filesystem::create_directories(work / "source", code);
    {
      std::ofstream source(work / "source" / "check.cc");
      source << kProgram;
      std::ofstream project(work / "source" / "CMakeLists.txt");
      project << kProject;
    }

    // Through the toolchain file, because that is what anything using this
    // will go through. A test that bypasses it tests something nobody uses.
    if (!runChecked(
            {.fProgram = context.fTools.fCmake,
             .fArguments =
                 {"-S", (work / "source").string(), "-B",
                  (work / "build").string(), "-G", "Ninja",
                  std::format("-DCMAKE_TOOLCHAIN_FILE={}",
                              toolchainFilePath(context.fLayout).string()),
                  "-DCMAKE_BUILD_TYPE=Release"}},
            "configuring the check")) {
      return false;
    }
    if (!runChecked({.fProgram = context.fTools.fCmake,
                     .fArguments = {"--build", (work / "build").string()}},
                    "building the check")) {
      return false;
    }

    std::optional<std::filesystem::path> object;
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(work / "build", code)) {
      if (entry.path().extension() == ".so") {
        object = entry.path();
        break;
      }
    }
    if (!object) {
      log::error("the check built no shared object");
      return false;
    }
    log::info("{}", object->string());

    // What the platform is: exactly the stubs this toolchain generated. A
    // library that needs anything else needs something Android will not
    // have.
    const std::filesystem::path stubs =
        context.fLayout.sysrootLib(context.target());
    std::set<std::string> platform;
    std::set<std::string> ignored;
    std::set<std::string> available;
    for (const auto &entry :
         std::filesystem::directory_iterator(stubs, code)) {
      if (entry.path().extension() != ".so") {
        continue;
      }
      platform.insert(entry.path().filename().string());
      dynamicSymbols(context, entry.path(), ignored, available);
    }
    log::info("the platform is {} libraries offering {} symbols",
              platform.size(), available.size());

    bool good = true;
    for (const std::string &needed : neededLibraries(context, *object)) {
      if (!platform.contains(needed)) {
        log::error("it says it needs {}, which is not one of the platform "
                   "libraries this toolchain knows about",
                   needed);
        good = false;
      } else {
        log::info("needs {}", needed);
      }
    }

    std::set<std::string> undefined;
    std::set<std::string> defined;
    dynamicSymbols(context, *object, undefined, defined);
    std::vector<std::string> missing;
    for (const std::string &symbol : undefined) {
      if (!available.contains(symbol)) {
        missing.push_back(symbol);
      }
    }
    if (!missing.empty()) {
      log::error("{} symbol(s) are left for somebody who is not there:",
                 missing.size());
      for (const std::string &symbol : missing) {
        log::error("  {}", symbol);
      }
      good = false;
    } else {
      log::info("all {} of its undefined symbols are ones the platform has",
                undefined.size());
    }

    // The one symbol worth naming: it is what the builtins exist for, and a
    // toolchain that resolved everything else and not this one has a
    // compiler-rt that did not arrive.
    if (undefined.contains("__emutls_get_address") &&
        !available.contains("__emutls_get_address")) {
      log::error("__emutls_get_address is unresolved, which means the "
                 "builtins were not linked in");
      good = false;
    }
    return good;
  };
  return step;
}

} // namespace mandk::steps
