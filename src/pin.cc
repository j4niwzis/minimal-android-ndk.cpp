export module mandk.pin;

import std;
import mandk.aosp;
import mandk.git;
import mandk.layout;
import mandk.log;
import mandk.manifest;
import mandk.manifest.io;

export namespace mandk {

[[nodiscard]] inline bool looksLikeCommit(std::string_view value) {
  return value.size() == 40 && std::ranges::all_of(value, [](char c) {
           return std::isxdigit(static_cast<unsigned char>(c)) != 0;
         });
}

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
  bool resolvedRefs = false;
  for (Source &source : manifest.fSources) {
    std::optional<std::string> commit;
    std::string from;
    if (!source.fProject.empty()) {
      const auto found = revisions.find(source.fProject);
      if (found == revisions.end()) {
        // Not every Android repository is part of a platform release. The
        // NDK is built from its own manifest branch, so a platform tag says
        // nothing about it.
        log::note("the {} manifest has no project {}; resolving {} against "
                  "its own remote",
                  tag, source.fProject, source.fName);
      } else {
        commit = found->second;
        from = std::format("the {} manifest", tag);
        // A release manifest names a revision, and that revision is usually
        // a ref rather than a commit. A ref pins nothing, so it is resolved
        // the rest of the way.
        if (!looksLikeCommit(*commit)) {
          log::debug("{} is at {} in the manifest", source.fProject, *commit);
          resolvedRefs = true;
          commit = git::resolve(source.fUrl, *commit);
          from = std::format("{}, resolved at the remote", from);
        }
      }
    }
    if (!commit) {
      // Either the source is not an AOSP project at all, or the platform
      // manifest does not carry it. HEAD is the last resort: it is whatever
      // the default branch says today, which is not a pin -- but writing the
      // commit it names is exactly what turns it into one.
      const std::string ref = source.fRef.empty() ? std::string("HEAD")
                                                  : source.fRef;
      if (source.fRef.empty()) {
        log::warn("{} names no ref; taking the head of its default branch. "
                  "Put a ref in the manifest to say which one you meant.",
                  source.fName);
      }
      commit = git::resolve(source.fUrl, ref);
      from = ref;
    }
    if (!commit) {
      complete = false;
      continue;
    }
    if (source.fCommit != *commit) {
      log::info("{}: {} -> {} ({})", source.fName,
                source.fCommit.empty() ? "unpinned"
                                       : source.fCommit.substr(0, 12),
                commit->substr(0, 12), from);
    }
    source.fCommit = *commit;
  }
  if (resolvedRefs) {
    log::info("the release manifest names refs rather than commits; each was "
              "resolved against its remote");
  }
  if (!complete) {
    log::error("nothing was written; the manifest is left as it was");
    return false;
  }
  return saveManifest(manifest);
}

} // namespace mandk
