export module mandk.steps.headers;

import std;
import mandk.copy;
import mandk.layout;
import mandk.log;
import mandk.manifest;
import mandk.plan;
import mandk.search;

export namespace mandk::steps {

[[nodiscard]] inline Step headers() {
  Step step;
  step.fName = "sysroot-headers";
  step.fSummary = "assemble usr/include from the platform checkouts";
  step.fNeeds = {"sources"};
  step.fKey = [](const Context &context) -> std::optional<std::string> {
    return context.baseKey();
  };
  step.fRun = [](Context &context) {
    const std::filesystem::path include = context.fLayout.sysrootInclude();
    std::error_code code;
    std::filesystem::create_directories(include, code);

    CopyReport report;
    for (const HeaderRule &rule : context.fManifest.fHeaders) {
      const std::filesystem::path root =
          context.fLayout.sourceOf(rule.fSource);
      std::vector<std::filesystem::path> from =
          search::directoriesEndingWith(root, rule.fFind);
      if (from.empty()) {
        log::error("{} has no directory ending in {}", rule.fSource, rule.fFind);
        log::error("  header layouts move between platform revisions; find "
                   "the new one and correct the manifest");
        return false;
      }
      if (!rule.fAll) {
        from.resize(1);
      }
      const std::filesystem::path into =
          rule.fInto.empty() ? include : include / rule.fInto;
      for (const std::filesystem::path &directory : from) {
        log::info("{} -> usr/include{}", directory.string(),
                  rule.fInto.empty() ? std::string() : "/" + rule.fInto);
        copyTree(directory, into, rule.fExclude, report);
      }
    }
    log::info("{} headers copied, {} already present, {} disagreed",
              report.fCopied, report.fSkipped, report.fConflicts);

    // What every rule together was supposed to produce. A sysroot that is
    // missing one of these compiles nothing, and saying so here is cheaper
    // than a compiler error a hundred steps later.
    bool complete = true;
    for (const HeaderRule &rule : context.fManifest.fHeaders) {
      for (const std::string &required : rule.fRequire) {
        if (!std::filesystem::exists(include / required, code)) {
          log::error("usr/include/{} is missing after the rule for {}",
                     required, rule.fFind);
          complete = false;
        }
      }
    }
    return complete;
  };
  return step;
}

} // namespace mandk::steps
