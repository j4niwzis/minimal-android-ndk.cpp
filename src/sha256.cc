export module mandk.sha256;

import std;

export namespace mandk {

// FIPS 180-4. The two tables are the fractional parts of the square roots of
// the first eight primes and of the cube roots of the first sixty-four, taken
// to 32 bits, which is how the standard states them.
class Sha256 {
public:
  void update(std::span<const std::byte> data) {
    for (const std::byte value : data) {
      fBlock[fFilled++] = static_cast<std::uint8_t>(value);
      if (fFilled == kBlockBytes) {
        this->compress();
        fFilled = 0;
      }
    }
    fLength += data.size();
  }
  void update(std::string_view data) {
    this->update(std::as_bytes(std::span(data.data(), data.size())));
  }

  [[nodiscard]] std::string hex() const {
    Sha256 copy = *this;
    copy.finish();
    std::string out;
    out.reserve(kDigestBytes * 2);
    for (const std::uint32_t word : copy.fState) {
      out += std::format("{:08x}", word);
    }
    return out;
  }

private:
  static constexpr std::size_t kBlockBytes = 64;
  static constexpr std::size_t kDigestBytes = 8;

  static constexpr std::uint32_t kInitial[kDigestBytes] = {
      0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  static constexpr std::uint32_t kRound[kBlockBytes] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
      0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
      0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
      0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
      0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
      0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

  void compress() {
    std::array<std::uint32_t, kBlockBytes> schedule{};
    for (std::size_t i = 0; i < 16; ++i) {
      schedule[i] = (static_cast<std::uint32_t>(fBlock[i * 4]) << 24) |
                    (static_cast<std::uint32_t>(fBlock[i * 4 + 1]) << 16) |
                    (static_cast<std::uint32_t>(fBlock[i * 4 + 2]) << 8) |
                    static_cast<std::uint32_t>(fBlock[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < kBlockBytes; ++i) {
      const std::uint32_t low = schedule[i - 15];
      const std::uint32_t high = schedule[i - 2];
      const std::uint32_t s0 = std::rotr(low, 7) ^ std::rotr(low, 18) ^ (low >> 3);
      const std::uint32_t s1 =
          std::rotr(high, 17) ^ std::rotr(high, 19) ^ (high >> 10);
      schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }
    std::array<std::uint32_t, kDigestBytes> working{};
    std::ranges::copy(fState, working.begin());
    for (std::size_t i = 0; i < kBlockBytes; ++i) {
      const std::uint32_t s1 = std::rotr(working[4], 6) ^
                               std::rotr(working[4], 11) ^
                               std::rotr(working[4], 25);
      const std::uint32_t choose =
          (working[4] & working[5]) ^ (~working[4] & working[6]);
      const std::uint32_t temp1 =
          working[7] + s1 + choose + kRound[i] + schedule[i];
      const std::uint32_t s0 = std::rotr(working[0], 2) ^
                               std::rotr(working[0], 13) ^
                               std::rotr(working[0], 22);
      const std::uint32_t majority = (working[0] & working[1]) ^
                                     (working[0] & working[2]) ^
                                     (working[1] & working[2]);
      const std::uint32_t temp2 = s0 + majority;
      working[7] = working[6];
      working[6] = working[5];
      working[5] = working[4];
      working[4] = working[3] + temp1;
      working[3] = working[2];
      working[2] = working[1];
      working[1] = working[0];
      working[0] = temp1 + temp2;
    }
    for (std::size_t i = 0; i < kDigestBytes; ++i) {
      fState[i] += working[i];
    }
  }

  void finish() {
    const std::uint64_t bits = fLength * 8;
    fBlock[fFilled++] = 0x80;
    if (fFilled > kBlockBytes - 8) {
      while (fFilled < kBlockBytes) {
        fBlock[fFilled++] = 0;
      }
      this->compress();
      fFilled = 0;
    }
    while (fFilled < kBlockBytes - 8) {
      fBlock[fFilled++] = 0;
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
      fBlock[fFilled++] = static_cast<std::uint8_t>((bits >> shift) & 0xff);
    }
    this->compress();
  }

  std::array<std::uint32_t, kDigestBytes> fState{
      kInitial[0], kInitial[1], kInitial[2], kInitial[3],
      kInitial[4], kInitial[5], kInitial[6], kInitial[7]};
  std::array<std::uint8_t, kBlockBytes> fBlock{};
  std::size_t fFilled = 0;
  std::uint64_t fLength = 0;
};

[[nodiscard]] inline std::string sha256(std::string_view data) {
  Sha256 hash;
  hash.update(data);
  return hash.hex();
}

[[nodiscard]] inline std::optional<std::string>
sha256File(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  Sha256 hash;
  std::array<char, 65536> buffer{};
  while (file.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
         file.gcount() > 0) {
    hash.update(std::string_view(buffer.data(),
                                 static_cast<std::size_t>(file.gcount())));
    if (file.eof()) {
      break;
    }
  }
  return hash.hex();
}

// A directory reduced to one digest: every regular file under it, in path
// order, contributing its relative name and its contents. Symlinks contribute
// their target rather than what they point at, so a tree that only differs in
// where a link goes does not hash the same.
[[nodiscard]] inline std::string sha256Tree(const std::filesystem::path &root) {
  std::vector<std::filesystem::path> entries;
  std::error_code code;
  for (const auto &entry : std::filesystem::recursive_directory_iterator(
           root, std::filesystem::directory_options::skip_permission_denied,
           code)) {
    entries.push_back(entry.path());
  }
  std::ranges::sort(entries);
  Sha256 hash;
  for (const auto &entry : entries) {
    const std::filesystem::path relative =
        std::filesystem::relative(entry, root, code);
    hash.update(relative.string());
    hash.update(std::string_view("\0", 1));
    if (std::filesystem::is_symlink(entry, code)) {
      hash.update(std::filesystem::read_symlink(entry, code).string());
    } else if (std::filesystem::is_regular_file(entry, code)) {
      const std::optional<std::string> digest = sha256File(entry);
      hash.update(digest.value_or("unreadable"));
    }
    hash.update(std::string_view("\n", 1));
  }
  return hash.hex();
}

} // namespace mandk
