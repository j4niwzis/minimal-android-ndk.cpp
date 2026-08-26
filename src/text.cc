export module mandk.text;

import std;

export namespace mandk::text {

[[nodiscard]] inline std::string_view trim(std::string_view value) {
  constexpr std::string_view kSpace = " \t\r\n";
  const std::size_t first = value.find_first_not_of(kSpace);
  if (first == std::string_view::npos) {
    return {};
  }
  return value.substr(first, value.find_last_not_of(kSpace) - first + 1);
}

[[nodiscard]] inline std::vector<std::string> split(std::string_view value,
                                                    char separator) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t next = value.find(separator, start);
    if (next == std::string_view::npos) {
      parts.emplace_back(value.substr(start));
      break;
    }
    parts.emplace_back(value.substr(start, next - start));
    start = next + 1;
  }
  return parts;
}

[[nodiscard]] inline std::string join(std::span<const std::string> parts,
                                      std::string_view separator) {
  std::string result;
  for (const auto &part : parts) {
    if (!result.empty()) {
      result += separator;
    }
    result += part;
  }
  return result;
}

[[nodiscard]] inline bool startsWith(std::string_view value,
                                     std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] inline bool endsWith(std::string_view value,
                                   std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

// Quoted the way a POSIX shell reads it. Nothing here is run through a shell
// -- the tool execs directly -- so this exists for the log line, which is
// meant to be copied out and re-run by hand.
[[nodiscard]] inline std::string shellQuote(std::string_view value) {
  const bool plain = !value.empty() &&
                     std::ranges::all_of(value, [](char c) {
                       return std::isalnum(static_cast<unsigned char>(c)) != 0 ||
                              std::string_view("._-/=:+,@").find(c) !=
                                  std::string_view::npos;
                     });
  if (plain) {
    return std::string(value);
  }
  std::string quoted = "'";
  for (const char c : value) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += '\'';
  return quoted;
}

[[nodiscard]] inline std::string commandLine(std::span<const std::string> argv) {
  std::string line;
  for (const auto &argument : argv) {
    if (!line.empty()) {
      line += ' ';
    }
    line += shellQuote(argument);
  }
  return line;
}

} // namespace mandk::text
