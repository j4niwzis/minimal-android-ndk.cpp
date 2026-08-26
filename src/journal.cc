export module mandk.journal;

import std;
import mandk.log;

export namespace mandk {

// What a step was last run with. A step states a key made of everything that
// decides its output -- the commits it reads, the target, the versions of the
// host tools it calls -- and is skipped while the key it would use now is the
// key it finished with.
//
// A step that cannot state such a key is run every time; that is the honest
// answer for one whose inputs are not enumerable.
class Journal {
public:
  explicit Journal(std::filesystem::path directory)
      : fDirectory(std::move(directory)) {}

  [[nodiscard]] std::optional<std::string> completedKey(
      std::string_view step) const {
    std::ifstream file(this->stamp(step));
    if (!file) {
      return std::nullopt;
    }
    std::string key;
    std::getline(file, key);
    return key;
  }

  [[nodiscard]] bool upToDate(std::string_view step,
                              const std::optional<std::string> &key) const {
    if (!key) {
      return false;
    }
    const std::optional<std::string> done = this->completedKey(step);
    return done && *done == *key;
  }

  void record(std::string_view step, const std::optional<std::string> &key) {
    if (!key) {
      return;
    }
    std::error_code code;
    std::filesystem::create_directories(fDirectory, code);
    std::ofstream file(this->stamp(step), std::ios::trunc);
    if (!file) {
      log::warn("cannot write the stamp for {}", step);
      return;
    }
    file << *key << '\n';
  }

  void forget(std::string_view step) {
    std::error_code code;
    std::filesystem::remove(this->stamp(step), code);
  }

private:
  [[nodiscard]] std::filesystem::path stamp(std::string_view step) const {
    return fDirectory / std::format("{}.stamp", step);
  }

  std::filesystem::path fDirectory;
};

} // namespace mandk
