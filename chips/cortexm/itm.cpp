#include "cortexm/itm.hpp"
#include <nlohmann/json.hpp>
#include "stest/node.hpp"

namespace stest {
namespace {
constexpr uint32_t kTcrWritable = 0x007F0F1Fu;  // ITMENA..SWOENA, TSPrescale, GTSFREQ, TraceBusID

// Length of the valid UTF-8 sequence at s[i], or 0.
size_t utf8_len(const std::string& s, size_t i) {
  auto c = static_cast<unsigned char>(s[i]);
  size_t n = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
  if (n == 0 || i + n > s.size()) return 0;
  for (size_t k = 1; k < n; ++k)
    if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return 0;
  if (n == 3) {
    auto c1 = static_cast<unsigned char>(s[i + 1]);
    if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) return 0;
  } else if (n == 4) {
    auto c1 = static_cast<unsigned char>(s[i + 1]);
    if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) return 0;
  }
  return n;
}

std::string sanitize_utf8(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    size_t n = utf8_len(s, i);
    if (n) { out.append(s, i, n); i += n; }
    else { out += "\xEF\xBF\xBD"; ++i; }
  }
  return out;
}
}  // namespace

Itm::Itm(Clock& clock, const CoreDebug& dbg, bool debugger_attached)
    : clock_(clock), dbg_(dbg), attached_(debugger_attached) {
  reset();
}

void Itm::reset() {
  ter_ = attached_ ? 1u : 0u;
  tcr_ = attached_ ? kTcrItmEna : 0u;
  tpr_ = 0;
  locked_ = !attached_;
  for (auto& l : line_) l.clear();
}

void Itm::emit(unsigned port, std::string text) {
  emit_event(clock_.now_ns(), {{"kind", "itm"}, {"port", port}, {"text", sanitize_utf8(text)}});
}

void Itm::put_byte(unsigned port, uint8_t b) {
  std::string& l = line_[port];
  if (b == '\n') {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    emit(port, std::move(l));
    l.clear();
    return;
  }
  l.push_back(static_cast<char>(b));
  if (l.size() >= kMaxLine) {
    emit(port, std::move(l));
    l.clear();
  }
}

uint32_t Itm::read(uint32_t off, unsigned width) {
  if ((off & 3u) + width > 4) throw UnmodelledRegister(off, width, false);
  if (off < 0x80) return 1;  // stimulus port: FIFOREADY
  switch (off & ~3u) {
    case kTer: return lane_extract(ter_, off, width);
    case kTpr: return lane_extract(tpr_, off, width);
    case kTcr: return lane_extract(tcr_, off, width);  // BUSY (bit 23) is always 0
    case kLsr: return lane_extract(locked_ ? 3u : 1u, off, width);  // SLI | SLK
  }
  throw UnmodelledRegister(off, width, false);
}

void Itm::write(uint32_t off, unsigned width, uint32_t v) {
  if ((off & 3u) + width > 4) throw UnmodelledRegister(off, width, true);
  if (off < 0x80) {
    unsigned port = off / 4;
    if (!(dbg_.trcena() && (tcr_ & kTcrItmEna) && (ter_ >> port & 1u))) return;  // dropped
    for (unsigned i = 0; i < width; ++i) put_byte(port, static_cast<uint8_t>(v >> (8 * i)));
    return;
  }
  switch (off & ~3u) {
    case kTer: ter_ = lane_merge(ter_, off, width, v); return;
    case kTpr: tpr_ = lane_merge(tpr_, off, width, v) & 0xFu; return;
    case kTcr: tcr_ = lane_merge(tcr_, off, width, v) & kTcrWritable; return;
    case kLar: locked_ = lane_merge(0, off, width, v) != kLarKey; return;
  }
  throw UnmodelledRegister(off, width, true);
}

std::vector<std::pair<int, std::string>> Itm::take_partial() {
  std::vector<std::pair<int, std::string>> out;
  for (size_t p = 0; p < line_.size(); ++p)
    if (!line_[p].empty()) {
      out.emplace_back(static_cast<int>(p), sanitize_utf8(line_[p]));
      line_[p].clear();
    }
  return out;
}

}  // namespace stest
