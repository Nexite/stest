/* stest control protocol (node <-> Python), version 2.
 *
 * Transport: a connected SOCK_STREAM socket whose fd number the node gets in the
 * environment variable STEST_CTRL (Python passes one end of a socketpair). With
 * STEST_CTRL unset the node runs standalone (see node_main.cpp).
 *
 * Framing, all integers little-endian:
 *     u32 len | u8 type | payload[len]        (len = payload bytes, header excluded)
 *
 *   type                dir        payload
 *   1  HELLO            node->py   u32 proto_version (=2), u64 anchor_addr
 *                                  (runtime address of `stest_symbol_anchor`)
 *   2  RUN_UNTIL        py->node   u64 t_ns, u32 n_msgs, then per inbox message:
 *                                  u64 t_ns, u16 json_len, json {"model":"GPIOA",
 *                                  "op":"drive_pin","args":{...}}           (v2: inbox)
 *   3  STOPPED          node->py   u64 t_ns (node's virtual time), u32 n_events, then per
 *                                  event: u64 t_ns, u16 json_len, json (utf-8)
 *   4  READ_MEM         py->node   u64 addr, u32 len            -> 5 MEM: bytes[len]
 *   6  WRITE_MEM        py->node   u64 addr, u32 len, bytes     -> 7 OK (empty)
 *   8  MODEL_CALL       py->node   u16 json_len, json {"model":"GPIOA","op":"drive_pin",
 *                                  "args":{...}}                -> 9 MODEL_RESULT: json value
 *   10 ERROR            node->py   u16 msg_len, msg (utf-8 "stest: ..."), u32 n_events,
 *                                  events encoded as in STOPPED (v2); the node then exits 70
 *   11 SHUTDOWN         py->node   (empty); the node exits 0
 *   12 REQ_ERROR        node->py   utf-8 message: READ_MEM / WRITE_MEM / MODEL_CALL failed
 *                                  (bad address, unknown model/op, bad args); the node
 *                                  stays alive. (Extension to the Plan 1 table.)
 *
 * Inbox (v2, Python Node.post): RUN_UNTIL's messages are applied in order, on the loop thread
 * with the firmware parked, before the node runs to t_ns: each calls
 * Peripheral::deliver(op, args, t) on the named model, which schedules its effect as a Clock
 * event at t. Inbox t must be >= the node's current time (t == now is allowed). A message the
 * node cannot apply is a strict abort (ERROR, exit 70), never a REQ_ERROR: RUN_UNTIL has no
 * per-message reply and a bad message is a harness bug. Messages:
 *   stest: inbox message for <model> at t=<t> ns is in the past (now <now> ns; op <op>)
 *   stest: inbox message for <model> at t=<t> ns (op <op>): unknown model
 *   stest: inbox message for <model> at t=<t> ns (op <op>): unknown op: <op>
 *   stest: inbox message for <model> at t=<t> ns (op <op>): <the model's exception text>
 *
 * ERROR events (v2): every event still pending when the node aborted (those of the quantum
 * that aborted), followed by the unterminated ITM / stdout lines as events with
 * "partial":true (the same lines are also written to stderr as "stest: ITM unterminated ..." /
 * "stest: stdout unterminated ..." for the crash report; Python takes them from ERROR then).
 *
 * Events JSON, e.g. {"kind":"gpio","level":1,"pin":5,"port":"A"} (keys sorted).
 * Firmware main returning is a strict error: ERROR "stest: firmware main returned ...".
 * EOF on the control socket makes the node exit 0 quietly.
 */
#ifndef STEST_PROTOCOL_H
#define STEST_PROTOCOL_H

#define STEST_PROTO_VERSION 2u

enum stest_msg_type {
  STEST_MSG_HELLO = 1,
  STEST_MSG_RUN_UNTIL = 2,
  STEST_MSG_STOPPED = 3,
  STEST_MSG_READ_MEM = 4,
  STEST_MSG_MEM = 5,
  STEST_MSG_WRITE_MEM = 6,
  STEST_MSG_OK = 7,
  STEST_MSG_MODEL_CALL = 8,
  STEST_MSG_MODEL_RESULT = 9,
  STEST_MSG_ERROR = 10,
  STEST_MSG_SHUTDOWN = 11,
  STEST_MSG_REQ_ERROR = 12
};

/* Largest payload either side accepts (sanity bound against a corrupt stream). */
#define STEST_MAX_PAYLOAD (64u * 1024u * 1024u)

#ifdef __cplusplus
#include "stest/node.hpp"
namespace stest {
/* Control mode: runs `fw` on a Node of `chip`, serving the protocol on `fd` until
 * SHUTDOWN/EOF (exit 0), a strict abort (ERROR, exit 70) or firmware exit (ERROR, 70).
 * Never returns. */
[[noreturn]] void run_control_channel(int fd, const ChipDesc& chip, FirmwareMain fw,
                                      const void* anchor);
}  // namespace stest
#endif

#endif
