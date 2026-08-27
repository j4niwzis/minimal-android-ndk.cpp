export module mandk.steps.framework;

import std;
import mandk.layout;
import mandk.log;
import mandk.plan;
import mandk.process;
import mandk.search;
import mandk.sha256;
import mandk.text;

export namespace mandk::steps {

// The resource package of the platform.
//
// Every application's resources are linked against this: it is what
// @android:string/ok resolves through. There is no android.jar here and none
// is needed -- aapt2 takes the platform's resource package directly, which
// is what this builds.
//
// It is the last thing an APK needs that neither a distribution nor a
// compiler can provide, because it is the platform's own resources rather
// than a tool.

// Feature flags, out of the manifest that uses them.
//
// Modern platform resources gate elements on flags -- android:featureFlag --
// and aapt2 refuses to link a manifest whose flags it has not been given a
// value for. What the values should be is a decision: they are given as on,
// so that the resource package describes the platform with its flagged
// pieces present, which is the platform an application will meet.
[[nodiscard]] inline std::vector<std::string>
featureFlags(const std::filesystem::path &manifest) {
  std::ifstream file(manifest);
  const std::string text((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
  std::set<std::string> names;
  constexpr std::string_view kAttribute = "android:featureFlag=\"";
  std::size_t cursor = 0;
  while ((cursor = text.find(kAttribute, cursor)) != std::string::npos) {
    const std::size_t from = cursor + kAttribute.size();
    const std::size_t to = text.find('"', from);
    if (to == std::string::npos) {
      break;
    }
    std::string name = text.substr(from, to - from);
    // A leading ! is "when this flag is off"; the flag is the same flag.
    if (name.starts_with("!")) {
      name = name.substr(1);
    }
    if (!name.empty()) {
      names.insert(name);
    }
    cursor = to;
  }
  std::vector<std::string> flags;
  for (const std::string &name : names) {
    flags.push_back(std::format("{}=true", name));
  }
  return flags;
}

[[nodiscard]] inline Step frameworkRes() {
  Step step;
  step.fName = "framework-res";
  step.fSummary = "link the platform's resource package, which every "
                  "application's resources are linked against";
  step.fNeeds = {"sources"};
  step.fKey = [](const Context &context) -> std::optional<std::string> {
    const CommandResult version =
        run({.fProgram = context.fTools.fAapt2, .fArguments = {"version"}});
    if (!version.ok()) {
      return std::nullopt;
    }
    Sha256 hash;
    hash.update(context.baseKey());
    hash.update(firstLine(version.fOutput));
    return hash.hex();
  };
  step.fRun = [](Context &context) {
    if (!haveProgram(context.fTools.fAapt2)) {
      log::error("{} is not on PATH. It is a package on most distributions; "
                 "this builds what is not.",
                 context.fTools.fAapt2);
      return false;
    }
    const std::filesystem::path base = context.fLayout.sourceOf("base");
    const std::optional<std::filesystem::path> resources =
        search::directory(base, "core/res/res");
    if (!resources) {
      log::error("frameworks/base has no core/res/res, which is the "
                 "platform's resources");
      return false;
    }
    const std::filesystem::path manifest =
        resources->parent_path() / "AndroidManifest.xml";
    std::error_code code;
    if (!std::filesystem::exists(manifest, code)) {
      log::error("{} is missing", manifest.string());
      return false;
    }

    const std::filesystem::path work = context.fLayout.buildOf("framework");
    std::filesystem::remove_all(work, code);
    std::filesystem::create_directories(work, code);
    const std::filesystem::path compiled = work / "resources.zip";

    if (!runChecked({.fProgram = context.fTools.fAapt2,
                     .fArguments = {"compile", "--dir", resources->string(),
                                    "-o", compiled.string()}},
                    "compiling the platform's resources")) {
      return false;
    }

    // --package-id 0x01 is the platform's own package identifier, and aapt2
    // will only accept an identifier below 0x7f when told the reservation is
    // deliberate. It cannot be combined with --shared-lib, which is a
    // different thing that also sounds right.
    //
    // The minimum SDK is 26 or higher because the platform's own icons are
    // adaptive icons, and aapt2 rejects those for anything older.
    const std::filesystem::path apk = context.fLayout.tools() / "framework-res.apk";
    std::filesystem::create_directories(apk.parent_path(), code);
    std::vector<std::string> arguments{
        "link", "-o", apk.string(), "--manifest", manifest.string(),
        "-R", compiled.string(),
        "--package-id", "0x01", "--allow-reserved-package-id",
        "--auto-add-overlay", "--no-version-vectors",
        "--min-sdk-version", "26",
        "--target-sdk-version", std::to_string(context.fManifest.fTargetSdk),
        "--private-symbols", "com.android.internal"};
    const std::vector<std::string> flags = featureFlags(manifest);
    if (!flags.empty()) {
      log::info("the manifest gates {} element(s) on feature flags; all are "
                "linked as on",
                flags.size());
      arguments.push_back("--feature-flags");
      arguments.push_back(text::join(flags, ","));
    }
    if (!runChecked({.fProgram = context.fTools.fAapt2,
                     .fArguments = std::move(arguments)},
                    "linking the platform's resource package")) {
      return false;
    }

    const std::uintmax_t size = std::filesystem::file_size(apk, code);
    if (code || size == 0) {
      log::error("{} is empty after linking it", apk.string());
      return false;
    }
    log::info("{} ({} KiB)", apk.string(), size / 1024);
    return true;
  };
  return step;
}

} // namespace mandk::steps
