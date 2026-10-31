/* stest control protocol (node <-> Python), version 1.
 *
 * Transport: a connected SOCK_STREAM socket whose fd number the node gets in the
 * environment variable STEST_CTRL (Python passes one end of a socketpair). With
 * STEST_CTRL unset the node runs standalone (see node_main.cpp).
 *
 * Framing, all integers little-endian:
 *     u32 len | u8 type | payload[len]        (len = payload bytes, header excluded)
 *
 *   type                dir        payload
 *   1  HELLO            node->py   u32 proto_version (=1), u64 anchor_addr
 *                                  (runtime address of `stest_symbol_anchor`)
 *   2  RUN_UNTIL        py->node   u64 t_ns
 *   3  STOPPED          node->py   u64 t_ns (node's virtual time), u32 n_events, then per
 *                                  event: u64 t_ns, u16 json_len, json (utf-8)
 *   4  READ_MEM         py->node   u64 addr, u32 len            -> 5 MEM: bytes[len]
 *   6  WRITE_MEM        py->node   u64 addr, u32 len, bytes     -> 7 OK (empty)
 *   8  MODEL_CALL       py->node   u16 json_len, json {"model":"GPIOA","op":"drive_pin",
 *                                  "args":{...}}                -> 9 MODEL_RESULT: json value
 *   10 ERROR            node->py   utf-8 "stest: ..." message; the node then exits 70
 *   11 SHUTDOWN         py->node   (empty); the node exits 0
 *   12 REQ_ERROR        node->py   utf-8 message: READ_MEM / WRITE_MEM / MODEL_CALL failed
 *                                  (bad address, unknown model/op, bad args); the node
 *                                  stays alive. (Extension to the Plan 1 table.)
 *
 * Events JSON, e.g. {"kind":"gpio","level":1,"pin":5,"port":"A"} (keys sorted).
 * Firmware main returning is a strict error: ERROR "stest: firmware main returned ...".
 * EOF on the control socket makes the node exit 0 quietly.
 */
#ifndef STEST_PROTOCOL_H
#define STEST_PROTOCOL_H

#define STEST_PROTO_VERSION 1u

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
