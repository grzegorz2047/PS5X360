// RECONSTRUCTED for the 1.0.2 source rebuild (tools/source-1.0.2): the original file is the
// client of disc-helper.elf (127.0.0.1:9302), not published, and its protocol is unknown.
// This client never connects, so a disc the title cannot open directly is reported as such.
#pragma once
#include <cerrno>
#include <cstdint>
#include <utility>
#include <vector>

namespace xbox360ps5::disc_relay {
constexpr int kPort = 9302;
class Client {
 public:
  bool Connect() { error_ = ECONNREFUSED; return false; }
  bool List(std::vector<std::pair<int, uint64_t>>&) { error_ = ENOTCONN; return false; }
  bool Read(int, uint64_t, void*, uint32_t) { error_ = ENOTCONN; return false; }
  int error() const { return error_; }

 private:
  int error_ = 0;
};
}  // namespace xbox360ps5::disc_relay
