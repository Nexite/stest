#pragma once
// STM32L4 (L496) bxCAN controllers CAN1 (master) and CAN2 (slave), RM0351 section 55
// (bxCAN). Register layout and bits follow ST's stm32l496xx.h (CAN_TypeDef, CAN_MCR_*,
// CAN_MSR_*, CAN_TSR_*, CAN_RFxR_*, CAN_IER_*, CAN_ESR_*, CAN_BTR_*, CAN_TIxR_*, ...):
//   MCR 0x00  MSR 0x04  TSR 0x08  RF0R 0x0C  RF1R 0x10  IER 0x14  ESR 0x18  BTR 0x1C
//   TX mailbox m (0-2): TIxR 0x180+0x10m, TDTxR +4, TDLxR +8, TDHxR +0xC
//   RX FIFO f (0-1) output mailbox: RIxR 0x1B0+0x10f, RDTxR +4, RDLxR +8, RDHxR +0xC
//   filters (CAN1's block only): FMR 0x200, FM1R 0x204, FS1R 0x20C, FFA1R 0x214, FA1R 0x21C,
//   bank i registers FiR1/FiR2 at 0x240+8i (i = 0..27).
// Reset values (RM0351 55.9): MCR 0x00010002 (SLEEP, DBF), MSR 0x00000C02 (SLAK, SAMP, RX),
// TSR 0x1C000000 (TME0-2), BTR 0x01230000, FMR 0x2A1C0E01 (FINIT, CAN2SB = 14); other
// registers 0 (TX/RX mailboxes and filter banks are undefined at reset: 0 here).
// Gated by RCC APB1ENR1 CAN1EN (bit 25) / CAN2EN (bit 26) (part TOML).
//
// params: {"role": "master" | "slave" (required), "master": "CAN1" (slave only, default
//   "CAN1"), "irq": {"tx": n, "rx0": n, "rx1": n, "sce": n} (missing key: no vector)}.
// The slave has no filter registers: its 0x200-0x31F accesses abort as unmodelled (they live
// in CAN1's block, RM0351 55.4); it finds the master with ModelContext::get<Bxcan>(master)
// in connect() and uses banks CAN2SB..27 of the master's filters. CAN1 uses banks
// 0..CAN2SB-1. Using CAN2's filters needs CAN1's clock (the firmware's accesses to CAN1's
// block are RCC-gated).
//
// ---- Operating modes (RM0351 55.4) ----
// mode: sleep (reset) | init | normal. The requested mode is MCR.INRQ ? init : MCR.SLEEP ?
// sleep : normal (INRQ wins over SLEEP: ST's HAL_CAN_Init sets INRQ and waits for INAK
// before it clears SLEEP, so from sleep INRQ alone enters init). Entering init or sleep is
// acknowledged at once (INAK / SLAK set at the MCR write; hardware first lets an ongoing
// bus transfer finish - the bus engine decides frames at SOF, see "TX" below). Entering
// normal (from init or sleep) waits for bus synchronisation: one Clock event 11 recessive
// bit times after the MCR write (bit time from BTR and PCLK1 at that write; the bus is
// assumed idle), at which INAK / SLAK clear. A new MCR write before then cancels it.
// MSR.SAMP/RX read 1 (recessive bus), TXM/RXM read 0.
// Mode event, at every mode change (the instant INAK/SLAK change):
//   {"kind":"can_mode","periph":"CAN1","mode":"sleep|init|normal","bitrate":<int|null>,
//    "abom":bool[,"reset":true]}
// bitrate (bit/s, integer division) only when entering normal. While in normal, a clock-tree
// change that changes the bit rate re-emits the event with the new bitrate (subscribed to
// ModelContext::clock_tree_changed; nothing clock-derived is cached otherwise: an ongoing
// 11-bit synchronisation wait keeps the PCLK1 of its MCR write). abom = MCR.ABOM at the event;
// an MCR write that changes ABOM while in normal mode re-emits the event (the bus engine runs
// bus-off recovery). "reset": true on MCR.RESET (always emitted, also when already in sleep).
//   bitrate = PCLK1 / ((BRP+1) x (1 + (TS1+1) + (TS2+1)))  (80 MHz, BRP 9, TS1 12, TS2 1: 500 000)
// BTR is write-protected outside initialization mode (RM0351 55.9.2 "accessible only in
// initialization mode"): a BTR write while MSR.INAK = 0 aborts (strict) instead of being
// ignored. Reads are allowed.
// MCR.RESET (master reset): every register except the filters returns to its reset value,
// mode sleep; requests already handed to the bus are withdrawn (can_tx_abort) and their
// results ignored; the error counters are zeroed and a new counter epoch starts (below).
// MCR.DBF is stored only.
//
// ---- TX mailboxes (RM0351 55.7.1) ----
// A mailbox is empty (TMEx = 1), held (TXRQ set outside normal mode, TMEx = 0) or requested
// (handed to the bus engine, TMEx = 0). Setting TIxR.TXRQ on an empty mailbox requests it:
// in normal mode at once, event
//   {"kind":"can_tx","periph":"CAN1","mailbox":m,"id":n,"ext":bool,"rtr":bool,"dlc":n,
//    "data":"<hex>","txfp":0|1,"nart":bool,"seq":k}
// (data: lowercase hex of the dlc bytes, empty for remote frames; txfp/nart: MCR.TXFP/NART at
// the request; seq: per-controller counter, +1 per can_tx event, never reused, also not
// across MCR.RESET). Outside normal mode the request is held (hardware keeps TXRQ pending
// and transmits once the controller is in normal mode) and requested, in TXRQ order, when
// the controller enters normal. Leaving normal with requested mailboxes withdraws each
// (can_tx_abort, below); an "aborted" result puts it back to held (a later normal entry, or
// the result itself if it arrives in normal mode, requests it again with a new seq); a
// normal result (its attempt was already on the wire) completes it as usual.
// TIxR/TDTxR/TDLxR/TDHxR are write-protected while the mailbox is not empty (RM0351 55.9.3):
// a write then aborts (strict). DLC > 8 at TXRQ aborts (classic frames with DLC 9-15 are not
// modelled). TDTxR.TGT is stored (time-triggered mode is not modelled); TIME reads 0.
// TSR.ABRQx on a pending mailbox: held -> completed at once as aborted (no event);
// requested -> event {"kind":"can_tx_abort","periph":"CAN1","mailbox":m,"seq":k} and the
// mailbox stays busy (TMEx = 0, ABRQx reads 1) until the engine's result. ABRQx on an empty
// mailbox has no effect.
// deliver("tx_result", {"mailbox":m,"seq":k,"ok":bool,"arb_lost"?:bool,"aborted"?:bool,
//   "error"?:null|"stuff"|"form"|"ack"|"bit_recessive"|"bit_dominant"|"crc",
//   "tec"?,"rec"?,"epoch"?}, t): at t (EOF, the failed attempt's post time, or the engine's
//   abort acknowledgement time). Counters (see "Fault confinement") are applied first, also
//   for a stale result. Ignored (stale) unless mailbox m is requested with this seq. Exactly
//   one outcome: ok; arb_lost; error; aborted.
//   ok:       RQCPx = TXOKx = TMEx = 1, ALSTx = TERRx = 0, ESR.LEC = 0.
//   arb_lost: TXOKx = 0, ALSTx = 1.     error: TXOKx = 0, TERRx = 1, ESR.LEC = code
//             (ack 3; stuff 1, form 2, bit_recessive 4, bit_dominant 5, crc 6), and MSR.ERRI
//             if IER.LECIE. With NART = 0 the request stays pending (the engine retries,
//             RQCPx stays 0); with NART = 1 the mailbox completes: RQCPx = TMEx = 1.
//   aborted:  for an ABRQ: RQCPx = TMEx = 1, TXOKx = 0 (ALST/TERR keep the last attempt's);
//             for a withdrawal on leaving normal: back to held (see above).
// Setting TXRQx clears RQCPx (and so TXOKx/ALSTx/TERRx), as on hardware (RM0351 TSR).
// Completion clears TIxR.TXRQ and TSR.ABRQx. Writing 1 to RQCPx clears RQCPx, TXOKx, ALSTx,
// TERRx. TSR.CODE = lowest-numbered empty mailbox, else the lowest-priority pending one;
// LOWx = mailbox x is the lowest-priority pending one while more than one is pending.
// Priority (MCR.TXFP): 0 = by identifier (CAN arbitration order of the IDs; equal: lower
// mailbox number first), 1 = by TXRQ order.
//
// ---- RX (RM0351 55.7.4 - 55.7.5) ----
// deliver("rx", {"id":n,"ext":bool,"rtr":bool,"dlc":0..8,"data":"<hex>","tec"?,"rec"?,
// "epoch"?}, t): a frame that ended (EOF) at t. At t: counters applied (always: the frame was
// received on the bus, even if dropped below); dropped unless the controller is in normal mode (the engine
// already only delivers to nodes in normal mode at SOF) or while FMR.FINIT = 1 (filter
// initialization mode deactivates reception); ESR.LEC = 0; then filtered by the active banks
// of this controller. Filter number (FMI) per RM0351 "Filter match index": filters are
// numbered per FIFO, bank by bank from the controller's first bank (CAN1: bank 0, CAN2:
// bank CAN2SB), active or not, 1 filter for a 32-bit mask bank, 2 for 32-bit list or 16-bit
// mask, 4 for 16-bit list (FiR1 low, FiR1 high, FiR2 low, FiR2 high). Several matches:
// 32-bit before 16-bit, then list before mask, then the lower bank / position. No match:
// dropped. The frame goes to the matching filter's FIFO (FFA1R): 3 deep; FMP = count; FULL
// set when the third is stored (cleared by software, rc_w1); a fourth sets FOVR and, with
// MCR.RFLM = 0, overwrites the newest message, with RFLM = 1 is discarded. RFOMx releases
// the output mailbox (no effect when empty). RIxR/RDTxR (DLC, FMI; TIME 0)/RDLxR/RDHxR show
// the oldest message (stale contents once the FIFO is empty).
// The FIFO registers are read-only (writes abort).
//
// ---- Filters (RM0351 55.7.4, 55.9.4) ----
// FM1R/FS1R/FFA1R are writable only with FMR.FINIT = 1; FiRx only with FINIT = 1 or bank i
// inactive (FA1R bit clear). Other writes abort (strict; hardware ignores them). FMR.CAN2SB
// > 28 aborts.
//
// ---- Fault confinement (RM0351 55.7.6 "Error management", ISO 11898-1) ----
// The Python bus engine owns TEC and REC (it decides errors, ACK, delivery and retries inside
// its lookahead window and must know error passive / bus-off for that) and delivers absolute
// values with "tec", "rec" (0..65535) and "epoch" (all three or none) on tx_result and rx, and:
// deliver("error", {"lec":"stuff"|"form"|"ack"|"bit_recessive"|"bit_dominant"|"crc","tec","rec",
//   "epoch"}, t): the controller received an error frame (receive error): counters, then LEC
//   (and MSR.ERRI if IER.LECIE).
// deliver("counters", {"tec","rec","epoch"}, t): counters only (bus-off recovery: 0, 0 at the
//   instant the engine lets the node rejoin; RM0351 requires at least 128 x 11 recessive bits).
// error/counters reject unknown keys (strict); bad values -> std::invalid_argument.
// Counters (and an "error"'s LEC) whose epoch differs from the controller's are dropped: the epoch counts MCR.RESETs
// (the engine counts them from can_mode "reset"), so deliveries computed before a reset never
// overwrite the reset ESR. The model never changes TEC/REC itself; nothing else resets them
// (init/sleep mode keep them, as on hardware: only the recovery sequence and MCR.RESET clear
// them - RM0351 does not list another reset; documented assumption).
// ESR from the counters: TEC [23:16] = TEC & 0xFF (RM0351 CAN_ESR: "least significant byte of
// the 9-bit transmit error counter", so a bus-off TEC of 256..263 reads 0..7), REC [31:24] =
// min(REC, 255) (the engine saturates REC at 255; after a successful reception it decrements
// REC by 1 or resets it to 120 if it was above 128, RM0351 "Error management"),
// EWGF = TEC >= 96 or REC >= 96 (RM0351 "warning limit ... >= 96"), EPVF = TEC > 127 or
// REC > 127, BOFF = TEC > 255. MSR.ERRI is set when EWGF / EPVF / BOFF rises while EWGIE /
// EPVIE / BOFIE is set, and when an error stores a non-zero LEC while LECIE is set; falling
// flags never set it. ESR: only LEC is writable (HAL's CLEAR_BIT(ESR, LEC) writes the read-only
// fields back unchanged); EWGF/EPVF/BOFF/TEC/REC keep their hardware semantics (read-only).
// A bus-off controller keeps its mailboxes pending (the engine does not let it transmit until
// it rejoins). Bus-off recovery (ABOM = 1: automatic; 0: after INRQ set then cleared) is timed
// by the engine (see stest.can.bus), which posts "counters" at the rejoin instant - a Clock
// event, so polling ESR.BOFF is never fast-forwarded past it.
//
// ---- IRQs (levels, ModelContext::set_irq_source; RM0351 55.8) ----
//   tx  = TMEIE & (RQCP0 | RQCP1 | RQCP2)
//   rx0 = FMPIE0 & FMP0 != 0 | FFIE0 & FULL0 | FOVIE0 & FOVR0      (rx1 likewise)
//   sce = ERRIE & ERRI | WKUIE & WKUI | SLKIE & SLAKI
// SLAKI is set on entering sleep while SLKIE = 1 (rc_w1, like ERRI/WKUI).
//
// Known deviations / interpretations:
//  (a) INRQ/SLEEP during a reception: hardware finishes the in-flight frame before setting
//      INAK/SLAK and still receives it; here init/sleep is entered at once and that frame is
//      dropped (INAK/SLAK may rise up to one frame time early).
//  (b) ABRQ on a mailbox withdrawn by a mode exit completes when the engine's result arrives
//      (up to the lookahead + one frame later), not immediately.
//  (c) MCR.RESET also resets BTR/IER/ESR and the mailboxes: an interpretation beyond RM0351's
//      wording ("FMP bits and CAN_MCR initialized"); ST's HAL re-initialises them anyway.
//  (d) Unconfirmed filter readings: CAN2's FMI counts from 0 at bank CAN2SB; no frame is
//      received while FMR.FINIT = 1.
//
// Not modelled (strict NotModelled abort at the configuring write): MCR.TTCM (time-triggered
// mode), MCR.AWUM and IER.WKUIE (wake-up on bus activity: a sleeping controller does not
// see the bus here), BTR.LBKM / BTR.SILM (loop back / silent), DLC > 8 at TXRQ.
//
// call(): "state" -> {"mode", "bitrate" (null unless normal), "tec", "rec" (the delivered
//   counters, unclamped), "error_state": "active"|"passive"|"bus_off", "esr", "msr", "tsr"
//   (raw register values), "tx": [{"state": "empty"|"held"|"pending", "seq": k|null,
//   "abort": bool} x3], "fifo": [n0, n1]}.
//   "can_info" -> {"type": "bxcan", "role": "master" | "slave"}: identifies a CAN controller
//   (Python's Sim.start() checks every name wired with Sim.node(..., can={...}) with it).
// The Python harness compares the bitrate of a can_mode "normal" event with its bus's bit
// rate exactly (this integer division result vs CanBus.bitrate; no tolerance): any
// difference ends the simulation (NodeCrashed "bit rate X does not match bus ...").
#include <array>
#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>
#include "stest/clock.hpp"
#include "stest/model_context.hpp"
#include "stest/peripheral.hpp"

namespace stest::stm32l4 {

class Bxcan : public Peripheral {
 public:
  Bxcan(std::string name, const nlohmann::json& params, ModelContext& ctx);
  std::string_view name() const override { return name_; }
  uint32_t read(uint32_t off, unsigned width) override;
  void write(uint32_t off, unsigned width, uint32_t v) override;
  void reset() override;
  void connect(ModelContext& ctx) override;
  nlohmann::json call(std::string_view op, const nlohmann::json& args) override;
  void deliver(std::string_view op, const nlohmann::json& args, uint64_t t_ns) override;

  enum class Mode { Sleep, Init, Normal };
  Mode mode() const { return mode_; }
  // Configured bit rate (bit/s) from BTR and the current PCLK1; 0 if PCLK1 is 0.
  uint64_t bitrate() const;

 private:
  struct Frame {
    uint32_t id = 0;
    bool ext = false, rtr = false;
    uint8_t dlc = 0;
    std::array<uint8_t, 8> data{};
  };
  enum class Tx { Empty, Held, Requested };
  struct Mailbox {
    Tx st = Tx::Empty;
    uint64_t seq = 0;    // current request (Requested)
    uint64_t order = 0;  // TXRQ order (TXFP = 1 priority, held-request order)
    bool abort = false;     // firmware ABRQ pending
    bool withdraw = false;  // withdrawn on leaving normal, awaiting the engine's result
    bool rqcp = false, txok = false, alst = false, terr = false;
  };
  struct Fifo {
    std::array<Frame, 3> msg{};
    std::array<uint8_t, 3> fmi{};
    unsigned n = 0;
    bool full = false, ovr = false;
  };

  [[noreturn]] void not_modelled(const std::string& what) const;
  bool filter_reg(uint32_t aligned) const;
  void write_mcr(uint32_t v);
  void write_tsr(uint32_t lane_bits);
  void write_mailbox(unsigned m, uint32_t off, unsigned width, uint32_t v);
  void write_filter(uint32_t aligned, uint32_t off, unsigned width, uint32_t v);
  void master_reset();
  void apply_mode();
  void enter(Mode m);
  void request(unsigned m);
  void complete(unsigned m);
  void on_tx_result(unsigned m, uint64_t seq, int outcome, uint32_t lec);
  void on_rx(const Frame& f);
  bool match(const Frame& f, unsigned& fifo, uint8_t& fmi) const;
  void set_lec(uint32_t lec);
  void set_counters(uint32_t tec, uint32_t rec);
  uint64_t bit_ns() const;
  uint64_t arb_key(unsigned m) const;
  void show_fifo(unsigned f);
  void sync();  // TSR/RFxR/MSR from the state, then the IRQ levels
  void emit_mode(bool master_reset = false);

  std::string name_;
  Clock& clock_;
  ModelContext& ctx_;
  const ClockTree* clocks_ = nullptr;
  bool master_ = true;
  std::string master_name_;
  Bxcan* filters_ = nullptr;  // the model whose block holds the filter banks
  std::array<int, 4> irq_{-1, -1, -1, -1};
  std::array<bool, 4> level_{};
  std::array<char, 4> irq_tag_{};
  RegisterFile regs_;
  Mode mode_ = Mode::Sleep;
  bool syncing_ = false;
  EventId sync_ev_ = 0;
  uint64_t reported_bitrate_ = 0;
  std::array<Mailbox, 3> mb_{};
  std::array<Fifo, 2> fifo_{};
  uint64_t seq_ = 0, order_ = 0;
  bool slaki_ = false, erri_ = false;
  bool reported_abom_ = false;   // MCR.ABOM in the last can_mode event
  uint32_t tec_ = 0, rec_ = 0;   // fault-confinement counters, as delivered by the bus engine
  uint64_t epoch_ = 0;           // MCR.RESET count: deliveries of an older epoch carry stale counters
};

}  // namespace stest::stm32l4
