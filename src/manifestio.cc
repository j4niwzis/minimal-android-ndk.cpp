export module mandk.manifest.io;

import std;
import mandk.json;
import mandk.log;
import mandk.manifest;

// Reading and writing the manifest, apart from what a manifest is.
//
// The two were one module, and the cost of that was that everything which
// wanted to know what a Source is also loaded nlohmann's twenty-five
// thousand lines of templates -- eighteen of thirty translation units, none
// of which parse JSON. A module that is imported widely should be cheap to
// import.

export namespace mandk {

[[nodiscard]] inline std::optional<Manifest>
loadManifest(const std::filesystem::path &path) {
  std::ifstream file(path);
  if (!file) {
    log::error("cannot read the manifest at {}", path.string());
    return std::nullopt;
  }
  const std::string text((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
  json document;
  try {
    document = json::parse(text, nullptr, true, true);
  } catch (const std::exception &error) {
    log::error("{} is not valid JSON: {}", path.string(), error.what());
    return std::nullopt;
  }
  Manifest manifest;
  manifest.fPath = path;
  if (document.contains("platform")) {
    manifest.fPlatformTag = document["platform"].get<std::string>();
  }
  manifest.fTargetSdk = document.value("target-sdk", manifest.fTargetSdk);
  if (document.contains("target")) {
    const json &target = document["target"];
    manifest.fTarget.fTriple =
        target.value("triple", manifest.fTarget.fTriple);
    manifest.fTarget.fArch = target.value("arch", manifest.fTarget.fArch);
    manifest.fTarget.fAbi = target.value("abi", manifest.fTarget.fAbi);
    manifest.fTarget.fApi = target.value("api", manifest.fTarget.fApi);
  }
  for (const json &entry : document.value("sources", json::array())) {
    Source source;
    source.fName = entry.value("name", std::string());
    source.fUrl = entry.value("url", std::string());
    source.fProject = entry.value("project", std::string());
    source.fRef = entry.value("ref", std::string());
    source.fCommit = entry.value("commit", std::string());
    source.fOptional = entry.value("optional", false);
    source.fNote = entry.value("note", std::string());
    for (const json &item : entry.value("paths", json::array())) {
      source.fPaths.push_back(item.get<std::string>());
    }
    if (source.fName.empty() || source.fUrl.empty()) {
      log::error("a source in {} has no name or no url", path.string());
      return std::nullopt;
    }
    manifest.fSources.push_back(std::move(source));
  }
  for (const json &entry : document.value("stubs", json::array())) {
    StubLibrary stub;
    stub.fName = entry.value("library", std::string());
    if (entry.contains("source") && entry["source"].is_array()) {
      for (const json &name : entry["source"]) {
        stub.fSources.push_back(name.get<std::string>());
      }
    } else if (entry.contains("source")) {
      stub.fSources.push_back(entry["source"].get<std::string>());
    }
    stub.fFile = entry.value("file", std::string());
    stub.fPath = entry.value("path", std::string());
    for (const json &symbol : entry.value("verify", json::array())) {
      stub.fVerify.push_back(symbol.get<std::string>());
    }
    manifest.fStubs.push_back(std::move(stub));
  }
  for (const json &entry : document.value("packages", json::array())) {
    Package package;
    package.fName = entry.value("name", std::string());
    package.fSource = entry.value("source", package.fName);
    package.fBuilder = entry.value("builder", std::string("cmake"));
    package.fSubdirectory = entry.value("subdirectory", std::string());
    package.fNote = entry.value("note", std::string());
    for (const json &item : entry.value("options", json::array())) {
      package.fOptions.push_back(item.get<std::string>());
    }
    for (const json &item : entry.value("needs", json::array())) {
      package.fNeeds.push_back(item.get<std::string>());
    }
    for (const json &item : entry.value("produces", json::array())) {
      package.fProduces.push_back(item.get<std::string>());
    }
    if (package.fName.empty()) {
      log::error("a package in {} has no name", path.string());
      return std::nullopt;
    }
    manifest.fPackages.push_back(std::move(package));
  }
  for (const json &entry : document.value("headers", json::array())) {
    HeaderRule rule;
    rule.fSource = entry.value("source", std::string());
    rule.fFind = entry.value("find", std::string());
    rule.fInto = entry.value("into", std::string());
    rule.fAll = entry.value("all", false);
    for (const json &name : entry.value("exclude", json::array())) {
      rule.fExclude.push_back(name.get<std::string>());
    }
    for (const json &name : entry.value("require", json::array())) {
      rule.fRequire.push_back(name.get<std::string>());
    }
    if (rule.fSource.empty() || rule.fFind.empty()) {
      log::error("a header rule in {} has no source or nothing to find",
                 path.string());
      return std::nullopt;
    }
    manifest.fHeaders.push_back(std::move(rule));
  }
  return manifest;
}

[[nodiscard]] inline bool saveManifest(const Manifest &manifest) {
  json document;
  document["platform"] = manifest.fPlatformTag;
  document["target-sdk"] = manifest.fTargetSdk;
  document["target"] = {{"triple", manifest.fTarget.fTriple},
                        {"arch", manifest.fTarget.fArch},
                        {"abi", manifest.fTarget.fAbi},
                        {"api", manifest.fTarget.fApi}};
  document["sources"] = json::array();
  for (const Source &source : manifest.fSources) {
    json entry;
    entry["name"] = source.fName;
    entry["url"] = source.fUrl;
    if (!source.fProject.empty()) {
      entry["project"] = source.fProject;
    }
    if (!source.fRef.empty()) {
      entry["ref"] = source.fRef;
    }
    entry["commit"] = source.fCommit;
    if (source.fOptional) {
      entry["optional"] = true;
    }
    if (!source.fPaths.empty()) {
      entry["paths"] = source.fPaths;
    }
    if (!source.fNote.empty()) {
      entry["note"] = source.fNote;
    }
    document["sources"].push_back(std::move(entry));
  }
  document["headers"] = json::array();
  for (const HeaderRule &rule : manifest.fHeaders) {
    json entry;
    entry["source"] = rule.fSource;
    entry["find"] = rule.fFind;
    entry["into"] = rule.fInto;
    if (rule.fAll) {
      entry["all"] = true;
    }
    if (!rule.fExclude.empty()) {
      entry["exclude"] = rule.fExclude;
    }
    if (!rule.fRequire.empty()) {
      entry["require"] = rule.fRequire;
    }
    document["headers"].push_back(std::move(entry));
  }
  document["packages"] = json::array();
  for (const Package &package : manifest.fPackages) {
    json entry;
    entry["name"] = package.fName;
    if (package.fSource != package.fName) {
      entry["source"] = package.fSource;
    }
    entry["builder"] = package.fBuilder;
    if (!package.fSubdirectory.empty()) {
      entry["subdirectory"] = package.fSubdirectory;
    }
    if (!package.fOptions.empty()) {
      entry["options"] = package.fOptions;
    }
    if (!package.fNeeds.empty()) {
      entry["needs"] = package.fNeeds;
    }
    if (!package.fProduces.empty()) {
      entry["produces"] = package.fProduces;
    }
    if (!package.fNote.empty()) {
      entry["note"] = package.fNote;
    }
    document["packages"].push_back(std::move(entry));
  }
  document["stubs"] = json::array();
  for (const StubLibrary &stub : manifest.fStubs) {
    json entry;
    entry["library"] = stub.fName;
    if (stub.fSources.size() == 1) {
      entry["source"] = stub.fSources.front();
    } else {
      entry["source"] = stub.fSources;
    }
    if (!stub.fFile.empty()) {
      entry["file"] = stub.fFile;
    }
    if (!stub.fPath.empty()) {
      entry["path"] = stub.fPath;
    }
    if (!stub.fVerify.empty()) {
      entry["verify"] = stub.fVerify;
    }
    document["stubs"].push_back(std::move(entry));
  }
  // Written beside the manifest and moved over it, so an interrupted write
  // cannot leave the pins half replaced.
  const std::filesystem::path temporary = manifest.fPath.string() + ".new";
  {
    std::ofstream file(temporary, std::ios::trunc);
    if (!file) {
      log::error("cannot write {}", temporary.string());
      return false;
    }
    file << document.dump(2) << '\n';
  }
  std::error_code code;
  std::filesystem::rename(temporary, manifest.fPath, code);
  if (code) {
    log::error("cannot replace {}: {}", manifest.fPath.string(), code.message());
    return false;
  }
  return true;
}

} // namespace mandk
