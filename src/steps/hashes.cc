export module mandk.steps.hashes;

import std;
import mandk.json;
import mandk.layout;
import mandk.log;
import mandk.manifest;
import mandk.plan;
import mandk.sha256;

export namespace mandk::steps {

// What was built, from what. Two trees built from the same manifest on two
// machines can be compared line by line; a tree whose sysroot digest differs
// from the one recorded here has been edited by hand since.
[[nodiscard]] inline Step hashes() {
  Step step;
  step.fName = "hashes";
  step.fSummary = "record the commits and the digests of what was produced";
  step.fNeeds = {"sources"};
  step.fRun = [](Context &context) {
    json document;
    document["platform"] = context.fManifest.fPlatformTag;
    document["target"] = {{"triple", context.target().fTriple},
                          {"api", context.target().fApi},
                          {"abi", context.target().fAbi}};
    document["sources"] = json::object();
    for (const Source &source : context.fManifest.fSources) {
      document["sources"][source.fName] = source.fCommit;
    }
    document["trees"] = json::object();
    std::error_code code;
    for (const auto &[name, path] :
         std::initializer_list<std::pair<std::string, std::filesystem::path>>{
             {"sysroot", context.fLayout.sysroot()},
             {"prefix", context.fLayout.prefix()},
             {"clang-resource", context.fLayout.clangResource()},
             {"runtime-install", context.fLayout.runtimeInstall()}}) {
      if (std::filesystem::exists(path, code)) {
        document["trees"][name] = sha256Tree(path);
      }
    }
    document["libraries"] = json::object();
    const std::filesystem::path stubs =
        context.fLayout.sysrootLib(context.target());
    if (std::filesystem::exists(stubs, code)) {
      for (const auto &entry : std::filesystem::directory_iterator(stubs, code)) {
        if (entry.is_regular_file(code)) {
          document["libraries"][entry.path().filename().string()] =
              sha256File(entry.path()).value_or("unreadable");
        }
      }
    }
    const std::filesystem::path output =
        context.fLayout.root() / "manifest-hashes.json";
    std::ofstream file(output, std::ios::trunc);
    if (!file) {
      log::error("cannot write {}", output.string());
      return false;
    }
    file << document.dump(2) << '\n';
    log::info("{}", output.string());
    return true;
  };
  return step;
}

} // namespace mandk::steps
