export module mandk.process;

import std;
import mandk.log;
import mandk.posix;
import mandk.text;

export namespace mandk {

struct CommandResult {
  int fExitCode = -1;
  bool fStarted = false;
  bool fSignalled = false;
  // stdout and stderr in the order the child wrote them; they share one pipe
  // so that a compiler diagnostic keeps its place among the lines around it.
  std::string fOutput;

  [[nodiscard]] bool ok() const {
    return fStarted && !fSignalled && fExitCode == 0;
  }
};

struct Command {
  std::string fProgram;
  std::vector<std::string> fArguments;
  std::filesystem::path fDirectory;
  std::vector<std::pair<std::string, std::string>> fEnvironment;
  // Off for a child whose output belongs on the terminal as it happens, such
  // as a build that would otherwise look hung for several minutes.
  bool fCapture = true;
};

// The program is found on PATH by execvp and the arguments are passed as they
// are given: no shell is involved, so nothing here needs escaping.
[[nodiscard]] inline CommandResult run(const Command &command) {
  std::vector<std::string> printable;
  printable.push_back(command.fProgram);
  printable.insert(printable.end(), command.fArguments.begin(),
                   command.fArguments.end());
  // The environment is part of what was run. Leaving it out of the line meant
  // a failure could be read a dozen times without the reason being visible,
  // because the reason was a variable the command was given.
  std::string environment;
  for (const auto &[name, value] : command.fEnvironment) {
    environment += std::format("{}={} ", name, text::shellQuote(value));
  }
  log::debug("run {}{}{}", command.fDirectory.empty()
                               ? std::string()
                               : std::format("(in {}) ",
                                             command.fDirectory.string()),
             environment, text::commandLine(printable));

  std::vector<char *> argv;
  argv.reserve(printable.size() + 1);
  for (auto &part : printable) {
    argv.push_back(part.data());
  }
  argv.push_back(nullptr);

  CommandResult result;
  int channel[2] = {-1, -1};
  if (command.fCapture && pipe(channel) != 0) {
    log::error("pipe failed: {}", strerror(currentErrno()));
    return result;
  }

  const pid_t child = fork();
  if (child < 0) {
    log::error("fork failed: {}", strerror(currentErrno()));
    if (command.fCapture) {
      close(channel[0]);
      close(channel[1]);
    }
    return result;
  }
  if (child == 0) {
    if (command.fCapture) {
      close(channel[0]);
      dup2(channel[1], kStdoutFileNo);
      dup2(channel[1], kStderrFileNo);
      close(channel[1]);
    }
    if (!command.fDirectory.empty() && chdir(command.fDirectory.c_str()) != 0) {
      _exit(127);
    }
    for (const auto &[name, value] : command.fEnvironment) {
      setenv(name.c_str(), value.c_str(), 1);
    }
    execvp(argv[0], argv.data());
    // Only reached when the program could not be started at all, which is
    // what 127 means to a shell as well.
    _exit(127);
  }

  result.fStarted = true;
  if (command.fCapture) {
    close(channel[1]);
    std::array<char, 4096> buffer{};
    for (;;) {
      const ssize_t got = read(channel[0], buffer.data(), buffer.size());
      if (got <= 0) {
        break;
      }
      result.fOutput.append(buffer.data(), static_cast<std::size_t>(got));
    }
    close(channel[0]);
  }

  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (currentErrno() != kInterrupted) {
      log::error("waitpid failed: {}", strerror(currentErrno()));
      return result;
    }
  }
  if (exitedNormally(status)) {
    result.fExitCode = exitStatusOf(status);
  } else if (wasSignalled(status)) {
    result.fSignalled = true;
    result.fExitCode = 128 + terminatingSignal(status);
  }
  return result;
}

// Runs the command and reports its output when it fails, which is the only
// time the output of a successful compiler invocation is worth reading.
[[nodiscard]] inline bool runChecked(const Command &command,
                                     std::string_view what) {
  const CommandResult result = run(command);
  if (result.ok()) {
    return true;
  }
  std::vector<std::string> printable;
  printable.push_back(command.fProgram);
  printable.insert(printable.end(), command.fArguments.begin(),
                   command.fArguments.end());
  log::error("{} failed with status {}", what, result.fExitCode);
  std::string environment;
  for (const auto &[name, value] : command.fEnvironment) {
    environment += std::format("{}={} ", name, text::shellQuote(value));
  }
  log::error("  {}{}", environment, text::commandLine(printable));
  for (const auto &line : text::split(result.fOutput, '\n')) {
    if (!text::trim(line).empty()) {
      log::error("  | {}", line);
    }
  }
  return false;
}

// The first line of the output, which is what a version query or a
// git ls-remote answer is read from.
[[nodiscard]] inline std::string firstLine(std::string_view output) {
  const std::size_t end = output.find('\n');
  return std::string(text::trim(
      end == std::string_view::npos ? output : output.substr(0, end)));
}

// PATH is walked here rather than asked of a shell: the tool never starts
// one, and a missing host program should be named before anything is built.
[[nodiscard]] inline std::filesystem::path findProgram(std::string_view program) {
  const std::filesystem::path named(program);
  if (named.has_parent_path()) {
    return std::filesystem::exists(named) ? named : std::filesystem::path();
  }
  const char *const path = std::getenv("PATH");
  if (path == nullptr) {
    return {};
  }
  for (const auto &directory : text::split(path, ':')) {
    if (directory.empty()) {
      continue;
    }
    std::error_code code;
    const std::filesystem::path candidate =
        std::filesystem::path(directory) / named;
    if (std::filesystem::is_regular_file(candidate, code) ||
        std::filesystem::is_symlink(candidate, code)) {
      return candidate;
    }
  }
  return {};
}

[[nodiscard]] inline bool haveProgram(std::string_view program) {
  return !findProgram(program).empty();
}

} // namespace mandk
