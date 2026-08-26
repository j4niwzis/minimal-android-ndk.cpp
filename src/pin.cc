export module mandk.pin;

import std;
import mandk.aosp;
import mandk.git;
import mandk.layout;
import mandk.log;
import mandk.manifest;

export namespace mandk {

// Turns the manifest's refs into commits. AOSP repositories are resolved
// through the release manifest, which names a commit for every project it
// carries; everything else is resolved against its own remote.
[[nodiscard]] inline bool pin(Manifest &manifest, const Layout &layout,
                              const std::string &platformTag) {
  const std::string tag =
      platformTag.empty() ? manifest.fPlatformTag : platformTag;
  bool wantAosp = false;
  for (const Source &source : manifest.fSources) {
    wantAosp = wantAosp || !source.fProject.empty();
  }
  std::map<std::string, std::string> revisions;
  if (wantAosp) {
    if (tag.empty()) {
      log::error("no platform tag: pass --platform <android release tag>");
      return false;
    }
    const std::optional<std::string> document =
        aosp::fetchManifest(tag, layout.buildOf("platform-manifest"));
    if (!document) {
      return false;
    }
    revisions = aosp::projects(*document);
    log::info("the {} manifest names {} projects", tag, revisions.size());
    manifest.fPlatformTag = tag;
  }

  bool complete = true;
  for (Source &source : manifest.fSources) {
    std::optional<std::string> commit;
    if (!source.fProject.empty()) {
      const auto found = revisions.find(source.fProject);
      if (found == revisions.end()) {
        log::error("the {} manifest has no project {}", tag, source.fProject);
        complete = false;
        continue;
      }
      commit = found->second;
      // A release manifest states commits. One that states a branch name
      // instead pins nothing, so it is resolved the other way.
      if (commit->size() != 40 ||
          !std::ranges::all_of(*commit, [](char c) {
            return std::isxdigit(static_cast<unsigned char>(c)) != 0;
          })) {
        log::note("{} is at {} in the manifest, which is not a commit; asking "
                  "the remote",
                  source.fProject, *commit);
        commit = git::resolve(source.fUrl, *commit);
      }
    } else if (!source.fRef.empty()) {
      commit = git::resolve(source.fUrl, source.fRef);
    } else {
      log::error("{} has neither a project nor a ref to pin from",
                 source.fName);
      complete = false;
      continue;
    }
    if (!commit) {
      complete = false;
      continue;
    }
    if (source.fCommit != *commit) {
      log::info("{}: {} -> {}", source.fName,
                source.fCommit.empty() ? "unpinned" : source.fCommit.substr(0, 12),
                commit->substr(0, 12));
    }
    source.fCommit = *commit;
  }
  if (!complete) {
    log::error("nothing was written; the manifest is left as it was");
    return false;
  }
  return manifest.save();
}

} // namespace mandk
