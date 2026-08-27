export module mandk.steps.apksigner;

import std;
import mandk.layout;
import mandk.log;
import mandk.plan;
import mandk.process;
import mandk.search;
import mandk.sha256;
import mandk.text;

export namespace mandk::steps {

// The one part of making an APK that a distribution does not have.
//
// aapt2, zipalign, keytool and java come from the system; the signer does
// not, because it is an AOSP project rather than a package. It is Java, so
// building it is javac and jar and nothing else -- no Gradle, no Maven, no
// downloading of anything.
[[nodiscard]] inline Step apksigner() {
  Step step;
  step.fName = "apksigner";
  step.fSummary = "build the signer, which is the only APK tool a "
                  "distribution does not have";
  step.fNeeds = {"sources"};
  step.fKey = [](const Context &context) -> std::optional<std::string> {
    const CommandResult version =
        run({.fProgram = context.fTools.fJavac, .fArguments = {"-version"}});
    if (!version.ok()) {
      return std::nullopt;
    }
    Sha256 hash;
    hash.update(context.baseKey());
    hash.update(firstLine(version.fOutput));
    return hash.hex();
  };
  step.fRun = [](Context &context) {
    for (const std::string &program :
         {context.fTools.fJavac, context.fTools.fJar}) {
      if (!haveProgram(program)) {
        log::error("{} is not on PATH, and the signer is Java", program);
        return false;
      }
    }

    const std::filesystem::path apksig = context.fLayout.sourceOf("apksig");
    std::error_code code;
    if (!std::filesystem::exists(apksig, code)) {
      log::error("the apksig checkout is missing. It is optional in the "
                 "manifest, so ask for it: build apksigner fetches it.");
      return false;
    }

    // Two source roots: the library, and the command that uses it.
    std::vector<std::filesystem::path> roots;
    for (const std::string_view where : {"src/main/java", "src/apksigner/java"}) {
      const std::optional<std::filesystem::path> found =
          search::directory(apksig, where);
      if (!found) {
        log::error("apksig has no {}; the layout has changed", where);
        return false;
      }
      roots.push_back(*found);
    }

    const std::filesystem::path work = context.fLayout.buildOf("apksigner");
    const std::filesystem::path classes = work / "classes";
    const std::filesystem::path patched = work / "patched";
    std::filesystem::remove_all(work, code);
    std::filesystem::create_directories(classes, code);

    // What is left out, and why.
    //
    // The key management implementations talk to Amazon and Google and need
    // their SDKs, which are not here and are not wanted. The interfaces they
    // implement stay, because the signer's own factory names them.
    //
    // Conscrypt is not a file that can be left out: the command registers it
    // as a security provider in one line in the middle of its own main
    // class. That line is optional -- the signer works with the runtime's
    // providers -- so the file is copied with the line commented out and the
    // copy is compiled. The checkout is not touched: it is a git checkout,
    // and a tool that edits its own sources leaves a tree nobody can trust
    // the next time it is read.
    std::vector<std::string> sources;
    std::size_t skipped = 0;
    std::size_t edited = 0;
    for (const std::filesystem::path &root : roots) {
      for (const auto &entry : search::walk(root, false)) {
        if (entry.extension() != ".java") {
          continue;
        }
        const std::string path = entry.generic_string();
        if (path.find("/kms/aws/") != std::string::npos ||
            path.find("/kms/gcp/") != std::string::npos) {
          ++skipped;
          continue;
        }

        std::ifstream reading(entry);
        std::string text((std::istreambuf_iterator<char>(reading)),
                         std::istreambuf_iterator<char>());
        if (text.find("org.conscrypt") == std::string::npos) {
          sources.push_back(entry.string());
          continue;
        }

        const std::filesystem::path copy =
            patched / std::filesystem::relative(entry, root, code);
        std::filesystem::create_directories(copy.parent_path(), code);
        std::ofstream writing(copy, std::ios::trunc);
        std::size_t lines = 0;
        for (const auto &line : text::split(text, '\n')) {
          if (line.find("org.conscrypt") != std::string::npos) {
            writing << "// left out by minimal-android-ndk: " << line << '\n';
            ++lines;
          } else {
            writing << line << '\n';
          }
        }
        log::info("{}: {} line(s) about Conscrypt commented out",
                  entry.filename().string(), lines);
        sources.push_back(copy.string());
        ++edited;
      }
    }
    if (sources.empty()) {
      log::error("no Java sources under {} or {}", roots[0].string(),
                 roots[1].string());
      return false;
    }
    log::info("{} Java sources, {} left out, {} copied and edited",
              sources.size(), skipped, edited);

    // A list in a file rather than on the command line: there are a few
    // hundred of them and an argument list has a length.
    const std::filesystem::path listing = work / "sources.txt";
    {
      std::ofstream file(listing);
      for (const std::string &source : sources) {
        file << source << '\n';
      }
    }

    if (!runChecked({.fProgram = context.fTools.fJavac,
                     .fArguments = {"-nowarn", "-encoding", "UTF-8", "-d",
                                    classes.string(),
                                    std::format("@{}", listing.string())}},
                    "compiling the signer")) {
      return false;
    }

    const std::filesystem::path jar =
        context.fLayout.tools() / "apksigner.jar";
    std::filesystem::create_directories(jar.parent_path(), code);
    if (!runChecked(
            {.fProgram = context.fTools.fJar,
             .fArguments = {"--create", "--file", jar.string(), "--main-class",
                            "com.android.apksigner.ApkSignerTool", "-C",
                            classes.string(), "."}},
            "packing the signer")) {
      return false;
    }

    // It says its version or it is not a signer.
    const CommandResult version =
        run({.fProgram = context.fTools.fJava,
             .fArguments = {"-jar", jar.string(), "--version"}});
    if (!version.ok()) {
      log::error("{} was built and will not say its version:", jar.string());
      log::error("  {}", firstLine(version.fOutput));
      return false;
    }
    log::info("{} says it is {}", jar.string(), firstLine(version.fOutput));
    return true;
  };
  return step;
}

} // namespace mandk::steps
