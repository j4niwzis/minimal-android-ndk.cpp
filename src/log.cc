export module mandk.log;

import std;
import mandk.posix;

export namespace mandk::log {

enum class Level { kDebug, kInfo, kNote, kWarn, kError };

class Sink {
public:
  void setVerbose(bool verbose) { fVerbose = verbose; }
  [[nodiscard]] bool verbose() const { return fVerbose; }

  // A file every line is copied into, so that a failed step can be read after
  // the fact without re-running it.
  void setTranscript(const std::filesystem::path &path) {
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    fTranscript.open(path, std::ios::out | std::ios::app);
  }

  void write(Level level, std::string_view message) {
    if (level == Level::kDebug && !fVerbose) {
      return;
    }
    const std::string stamped =
        std::format("{:%H:%M:%S} {} {}", std::chrono::floor<std::chrono::seconds>(
                                             std::chrono::system_clock::now()),
                    tag(level), message);
    if (fTranscript.is_open()) {
      fTranscript << stamped << '\n';
      fTranscript.flush();
    }
    std::ostream &stream = level == Level::kError ? std::cerr : std::cout;
    stream << colour(level) << stamped << reset() << '\n';
    stream.flush();
  }

private:
  [[nodiscard]] static std::string_view tag(Level level) {
    switch (level) {
    case Level::kDebug: return "..";
    case Level::kInfo:  return "--";
    case Level::kNote:  return "**";
    case Level::kWarn:  return "!!";
    case Level::kError: return "XX";
    }
    return "--";
  }

  // Colour when the output is a terminal that has not asked to go without.
  // NO_COLOR is honoured for any value, which is what its specification says.
  [[nodiscard]] bool coloured() const {
    if (!fColourKnown) {
      const char *const noColour = std::getenv("NO_COLOR");
      const char *const term = std::getenv("TERM");
      fColoured = noColour == nullptr && term != nullptr &&
                  std::string_view(term) != "dumb" && isatty(kStdoutFileNo) == 1;
      fColourKnown = true;
    }
    return fColoured;
  }
  [[nodiscard]] std::string_view colour(Level level) const {
    if (!this->coloured()) {
      return {};
    }
    switch (level) {
    case Level::kDebug: return "\033[2m";
    case Level::kInfo:  return {};
    case Level::kNote:  return "\033[36m";
    case Level::kWarn:  return "\033[33m";
    case Level::kError: return "\033[31m";
    }
    return {};
  }
  [[nodiscard]] std::string_view reset() const {
    return this->coloured() ? "\033[0m" : std::string_view{};
  }

  bool fVerbose = false;
  mutable bool fColourKnown = false;
  mutable bool fColoured = false;
  std::ofstream fTranscript;
};

inline Sink &sink() {
  static Sink instance;
  return instance;
}

template <class... Args>
void debug(std::format_string<Args...> format, Args &&...args) {
  sink().write(Level::kDebug, std::format(format, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::format_string<Args...> format, Args &&...args) {
  sink().write(Level::kInfo, std::format(format, std::forward<Args>(args)...));
}
template <class... Args>
void note(std::format_string<Args...> format, Args &&...args) {
  sink().write(Level::kNote, std::format(format, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::format_string<Args...> format, Args &&...args) {
  sink().write(Level::kWarn, std::format(format, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::format_string<Args...> format, Args &&...args) {
  sink().write(Level::kError, std::format(format, std::forward<Args>(args)...));
}

} // namespace mandk::log
