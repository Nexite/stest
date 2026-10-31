#pragma once
// ITM (0xE0000000): stimulus ports, TER/TPR/TCR/LAR/LSR. Core-generic.
//
// Bytes written to a stimulus port (8/16/32-bit stores, little-endian bytes) accumulate in
// a per-port line buffer; '\n' completes a line and emits the event
// {"kind":"itm","periph":"ITM","port":N,"text":"<line without \n and a trailing \r>"} stamped with the
// virtual time of the store. A line longer than kMaxLine bytes is emitted early. Invalid
// UTF-8 bytes are replaced by U+FFFD (events are JSON).
//
// Output is delivered only while DEMCR.TRCENA, TCR.ITMENA and the port's TER bit are all
// set, as on hardware; otherwise the store is dropped.
//
// Software lock (CoreSight LAR/LSR, ARMv7-M): writing 0xC5ACCE55 to LAR unlocks, any other
// value locks. While locked, writes to TER/TPR/TCR are ignored (one stderr warning per run
// names the first one); stimulus-port stores and reads are unaffected. LSR reads 3 (SLI |
// SLK) locked, 1 unlocked. Bare board (debugger_attached = false): reset state locked with
// TER/TCR = 0, so firmware must unlock before enabling the ITM. Debugger attached (default):
// the debugger has unlocked the ITM and set TCR.ITMENA and TER port 0 (LSR = 1). Reads of a port return 1
// (FIFOREADY), so CMSIS ITM_SendChar never blocks. Registers the model does not cover
// (PID/CID, ...) abort in strict mode. TCR's TSENA/SYNCENA/TXENA/SWOENA/TSPrescale/
// TraceBusID bits are stored but have no effect (there is no trace port to drive).
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "cortexm/coredebug.hpp"
#include "stest/clock.hpp"
#include "stest/peripheral.hpp"

namespace stest {

class Itm : public Peripheral {
 public:
  static constexpr uint32_t kSize = 0x1000;
  static constexpr size_t kMaxLine = 4096;
  static constexpr uint32_t kTer = 0xE00, kTpr = 0xE40, kTcr = 0xE80, kLar = 0xFB0, kLsr = 0xFB4;
  static constexpr uint32_t kTcrItmEna = 1u, kLarKey = 0xC5ACCE55u;

  Itm(Clock& clock, const CoreDebug& dbg, bool debugger_attached);
  std::string_view name() const override { return "ITM"; }
  uint32_t read(uint32_t off, unsigned width) override;
  void write(uint32_t off, unsigned width, uint32_t v) override;
  void reset() override;

  // Unterminated per-port text (port, text) in port order; clears the buffers.
  std::vector<std::pair<int, std::string>> take_partial();

 private:
  void put_byte(unsigned port, uint8_t b);
  void emit(unsigned port, std::string text);
  Clock& clock_;
  const CoreDebug& dbg_;
  bool attached_;
  uint32_t ter_ = 0, tpr_ = 0, tcr_ = 0;
  bool locked_ = true;
  bool warned_locked_ = false;
  std::array<std::string, 32> line_;
};

}  // namespace stest
