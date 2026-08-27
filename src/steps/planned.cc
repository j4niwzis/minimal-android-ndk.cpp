export module mandk.steps.planned;

import std;
import mandk.plan;

export namespace mandk::steps {

// The rest of the toolchain, declared but not yet built by this tool. They
// are here so that `minimal-android-ndk plan` shows the whole road and so that the order
// they have to run in is written down once, in the place that will run them.
[[nodiscard]] inline Step planned(std::string name, std::string summary,
                                  std::vector<std::string> needs) {
  Step step;
  step.fName = std::move(name);
  step.fSummary = std::move(summary);
  step.fNeeds = std::move(needs);
  step.fReady = false;
  return step;
}

[[nodiscard]] inline std::vector<Step> remaining() {
  return {
  };
}

} // namespace mandk::steps
