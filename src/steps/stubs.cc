export module mandk.steps.stubs;

import std;
import mandk.json;
import mandk.layout;
import mandk.log;
import mandk.manifest;
import mandk.plan;
import mandk.process;
import mandk.search;
import mandk.sha256;
import mandk.text;

export namespace mandk::steps {

// The value AOSP uses for an API level that has not been assigned a number
// yet. It is read out of Soong rather than written down here: a preview
// codename in a map file has to resolve to whatever the checkout thinks the
// future is.
[[nodiscard]] inline std::optional<int>
futureApiLevel(const std::filesystem::path &soong) {
  const std::optional<std::filesystem::path> file =
      search::file(soong, "api_levels.go");
  if (!file) {
    return std::nullopt;
  }
  std::ifstream stream(*file);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.find("FutureApiLevel") == std::string::npos) {
      continue;
    }
    // Every run of digits on the line is considered: the name of the
    // constant and the architecture words around it carry digits of their
    // own, and the level is the one that is four digits or more.
    std::string digits;
    for (std::size_t i = 0; i <= line.size(); ++i) {
      const bool digit =
          i < line.size() && std::isdigit(static_cast<unsigned char>(line[i])) != 0;
      if (digit) {
        digits += line[i];
        continue;
      }
      if (digits.size() >= 4) {
        log::debug("soong calls the future API level {} ({})", digits,
                   file->string());
        return std::stoi(digits);
      }
      digits.clear();
    }
  }
  return std::nullopt;
}

// Every codename a map file introduces a symbol at. A numeric level needs no
// entry in the API map; a name does.
inline void collectCodenames(const std::filesystem::path &mapFile,
                             std::set<std::string> &names) {
  std::ifstream stream(mapFile);
  const std::string text((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
  constexpr std::string_view kIntroduced = "introduced";
  std::size_t cursor = 0;
  while ((cursor = text.find(kIntroduced, cursor)) != std::string::npos) {
    const std::size_t equals = text.find('=', cursor);
    if (equals == std::string::npos) {
      break;
    }
    // introduced=21 and introduced-arm64=VanillaIceCream both land here; the
    // architecture qualifier sits between the word and the equals sign.
    if (text.find_first_of(" \t\r\n;#", cursor) < equals) {
      cursor += kIntroduced.size();
      continue;
    }
    const std::size_t end = text.find_first_of(" \t\r\n;#", equals + 1);
    const std::string value =
        text.substr(equals + 1, end == std::string::npos
                                    ? std::string::npos
                                    : end - equals - 1);
    if (!value.empty() && !std::ranges::all_of(value, [](char c) {
          return std::isdigit(static_cast<unsigned char>(c)) != 0;
        })) {
      names.insert(value);
    }
    cursor = equals + 1;
  }
}

struct StubPlan {
  StubLibrary fLibrary;
  std::filesystem::path fMap;
};

[[nodiscard]] inline std::optional<std::vector<StubPlan>>
locateMaps(const Context &context) {
  std::vector<StubPlan> plans;
  for (const StubLibrary &stub : context.fManifest.fStubs) {
    std::optional<std::filesystem::path> map;
    for (const std::string &name : stub.fSources) {
      const std::filesystem::path root = context.fLayout.sourceOf(name);
      std::error_code code;
      if (!stub.fPath.empty() &&
          std::filesystem::exists(root / stub.fPath, code)) {
        map = root / stub.fPath;
      } else if (!stub.fFile.empty()) {
        map = search::file(root, stub.fFile);
      }
      if (map) {
        break;
      }
    }
    if (!map) {
      log::error("no map file for {}: looked for {} in {}", stub.soname(),
                 stub.fFile, [&stub] {
                   std::string joined;
                   for (const std::string &name : stub.fSources) {
                     joined += joined.empty() ? name : ", " + name;
                   }
                   return joined;
                 }());
      return std::nullopt;
    }
    log::debug("{} <- {}", stub.soname(), map->string());
    plans.push_back({stub, *map});
  }
  return plans;
}

[[nodiscard]] inline bool writeApiMap(const Context &context,
                                      std::span<const StubPlan> plans,
                                      const std::filesystem::path &output) {
  const std::filesystem::path ndk = context.fLayout.sourceOf("ndk");
  const std::optional<std::filesystem::path> platforms =
      search::file(ndk, "platforms.json");
  if (!platforms) {
    log::error("meta/platforms.json is not in the ndk checkout");
    return false;
  }
  json aliases = json::object();
  {
    std::ifstream stream(*platforms);
    json document;
    try {
      document = json::parse(stream);
    } catch (const std::exception &error) {
      log::error("{} is not valid JSON: {}", platforms->string(), error.what());
      return false;
    }
    if (document.contains("aliases")) {
      aliases = document["aliases"];
    }
  }

  std::set<std::string> codenames;
  for (const StubPlan &plan : plans) {
    collectCodenames(plan.fMap, codenames);
  }
  const std::optional<int> future =
      futureApiLevel(context.fLayout.sourceOf("soong"));
  for (const std::string &codename : codenames) {
    if (aliases.contains(codename)) {
      continue;
    }
    if (!future) {
      log::error("symbols are introduced at codename {}, and soong's future "
                 "API level could not be read to resolve it",
                 codename);
      return false;
    }
    log::info("codename {} resolves to the future API level {}", codename,
              *future);
    aliases[codename] = *future;
  }

  std::error_code code;
  std::filesystem::create_directories(output.parent_path(), code);
  std::ofstream file(output, std::ios::trunc);
  if (!file) {
    log::error("cannot write {}", output.string());
    return false;
  }
  file << aliases.dump(2) << '\n';
  return true;
}

// An empty shared object would let -lfoo succeed and leave every platform
// call undefined at run time, and a fabricated libc.so cannot even link
// libc++, which needs malloc, free and the pthread functions to be there. So
// each stub is generated from the platform's own symbol map and then read
// back to confirm the symbols and the soname arrived.
[[nodiscard]] inline bool verifyStub(const Context &context,
                                     const StubLibrary &stub,
                                     const std::filesystem::path &library) {
  const CommandResult symbols =
      run({.fProgram = context.fTools.fReadelf,
           .fArguments = {"--dyn-syms", "--wide", library.string()}});
  if (!symbols.ok()) {
    log::error("{} cannot be read back with {}", library.string(),
               context.fTools.fReadelf);
    return false;
  }
  bool good = true;
  for (const std::string &symbol : stub.fVerify) {
    if (symbols.fOutput.find(symbol) == std::string::npos) {
      log::error("{} does not export {}", stub.soname(), symbol);
      good = false;
    }
  }
  const CommandResult dynamic =
      run({.fProgram = context.fTools.fReadelf,
           .fArguments = {"--dynamic", "--wide", library.string()}});
  if (!dynamic.ok() ||
      dynamic.fOutput.find(std::format("[{}]", stub.soname())) ==
          std::string::npos) {
    log::error("{} has no SONAME", stub.soname());
    good = false;
  }
  return good;
}

[[nodiscard]] inline Step apiStubs() {
  Step step;
  step.fName = "api-stubs";
  step.fSummary = "generate and compile the Android API link stubs";
  step.fNeeds = {"sources", "sysroot-headers"};
  step.fKey = [](const Context &context) -> std::optional<std::string> {
    // The compiler is part of the key: the stubs are objects it produced.
    const CommandResult version =
        run({.fProgram = context.fTools.fClang, .fArguments = {"--version"}});
    if (!version.ok()) {
      return std::nullopt;
    }
    Sha256 hash;
    hash.update(context.baseKey());
    hash.update(firstLine(version.fOutput));
    for (const StubLibrary &stub : context.fManifest.fStubs) {
      hash.update(stub.fName);
      for (const std::string &name : stub.fSources) {
        hash.update(name);
      }
      hash.update(stub.fFile);
    }
    return hash.hex();
  };
  step.fRun = [](Context &context) {
    const std::optional<std::vector<StubPlan>> plans = locateMaps(context);
    if (!plans) {
      return false;
    }
    const std::filesystem::path work = context.fLayout.buildOf("stubs");
    const std::filesystem::path apiMap = work / "api-map.json";
    if (!writeApiMap(context, *plans, apiMap)) {
      return false;
    }

    const std::filesystem::path soong = context.fLayout.sourceOf("soong");
    const std::optional<std::filesystem::path> generator =
        search::directory(soong, "cc/ndkstubgen");
    if (!generator) {
      log::error("ndkstubgen is not in the soong checkout");
      return false;
    }
    // ndkstubgen imports symbolfile, which is its neighbour rather than its
    // child, so what belongs on PYTHONPATH is the directory holding both --
    // soong's cc -- and not the one above that.
    const std::filesystem::path pythonPath = generator->parent_path();

    const std::filesystem::path generated = work / "generated";
    const std::filesystem::path libraries =
        context.fLayout.sysrootLib(context.target());
    std::error_code code;
    std::filesystem::create_directories(generated, code);
    std::filesystem::create_directories(libraries, code);

    for (const StubPlan &plan : *plans) {
      const std::string name = plan.fLibrary.fName;
      const std::filesystem::path source =
          generated / std::format("{}.c", name);
      const std::filesystem::path version =
          generated / std::format("{}.map", name);
      const std::filesystem::path listing =
          generated / std::format("{}.symbols", name);
      const std::filesystem::path library =
          libraries / plan.fLibrary.soname();

      if (!runChecked(
              {.fProgram = context.fTools.fPython,
               .fArguments = {(*generator / "__init__.py").string(), "--api",
                              std::to_string(context.target().fApi), "--arch",
                              context.target().fArch, "--api-map",
                              apiMap.string(), plan.fMap.string(),
                              source.string(), version.string(),
                              listing.string()},
               .fEnvironment = {{"PYTHONPATH", pythonPath.string()}}},
              std::format("ndkstubgen for {}", plan.fLibrary.soname()))) {
        return false;
      }

      if (!runChecked(
              {.fProgram = context.fTools.fClang,
               .fArguments =
                   {std::format("--target={}", context.target().clangTarget()),
                    std::format("--sysroot={}",
                                context.fLayout.sysroot().string()),
                    "-fPIC", "-nostdlib", "-shared", source.string(),
                    std::format("-Wl,--version-script={}", version.string()),
                    std::format("-Wl,-soname,{}", plan.fLibrary.soname()), "-o",
                    library.string()}},
              std::format("compiling {}", plan.fLibrary.soname()))) {
        return false;
      }

      if (!verifyStub(context, plan.fLibrary, library)) {
        return false;
      }
      log::info("{}", library.string());
    }

    log::note("these are link-time stubs; Android supplies the real "
              "libraries, so none of them belongs in an APK");
    return true;
  };
  return step;
}

} // namespace mandk::steps
