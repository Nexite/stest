"""DBC support: encode/decode, DbcDevice periodic senders, decoded logs, message waits."""

from __future__ import annotations

from pathlib import Path

import pytest

from stest import CanFrame, Dbc, DbcDevice, Sim, StestError, WaitTimeout

DBC_PATH = Path(__file__).parent / "data" / "test.dbc"
BR = 500_000
IFS = 3 * 2_000


@pytest.fixture(scope="module")
def dbc() -> Dbc:
    return Dbc(DBC_PATH)


def test_round_trip_scaled_signed_enum(dbc):
    sig = {"Speed": 1234.5, "Torque": -100, "Temp": 21.5, "Mode": "Run", "Counter": 7}
    f = dbc.encode("Status", sig)
    assert f.id == 0x100 and not f.ext and len(f.data) == 8
    # raw: Speed 4938, Torque 12-bit two's complement of -100, Temp (21.5+40)/0.5 = 123, Mode 2
    assert int.from_bytes(f.data[:2], "little") == 4938
    assert dbc.decode(f) == ("Status", sig)
    # enum accepts the raw value too and decodes to the name
    f2 = dbc.encode("Status", {**sig, "Mode": 3})
    assert dbc.decode(f2)[1]["Mode"] == "Fault"
    # an enum value outside VAL_ decodes to the raw number
    raw = bytearray(f.data)
    raw[4] |= 0x70  # Mode bits 36..38 -> 7
    assert dbc.decode(CanFrame(0x100, bytes(raw)))[1]["Mode"] == 7


def test_extended_and_unknown_ids(dbc):
    f = dbc.encode("ExtInfo", {"Level": 513, "Flags": 5})
    assert f.ext and f.id == 0x18FF1234 and f.data == bytes([1, 2, 5, 0])
    assert dbc.decode(f) == ("ExtInfo", {"Level": 513, "Flags": 5})
    assert dbc.decode(CanFrame(0x18FF1234 & 0x7FF, b"\x00" * 4)) is None  # unrelated std id
    assert dbc.decode(CanFrame(0x7FF, b"\x00")) is None
    assert dbc.decode(CanFrame(0x7F0, ext=True, data=b"\x00" * 8)) is None  # ext/std mismatch
    assert dbc.decode(CanFrame(0x100, rtr=True, dlc=8)) is None


def test_multiplexed_decode(dbc):
    f0 = dbc.encode("Mux", {"Sel": 0, "Voltage": 12.34})
    assert dbc.decode(f0) == ("Mux", {"Sel": 0, "Voltage": 12.34})
    f1 = dbc.encode("Mux", {"Sel": 1, "Current": -1.5})
    name, sig = dbc.decode(f1)
    assert name == "Mux" and sig == {"Sel": 1, "Current": -1.5}
    assert "Voltage" not in sig and "Current" not in dbc.decode(f0)[1]


def test_encode_errors(dbc):
    with pytest.raises(StestError, match="Speed"):
        dbc.encode("Status", {"Speed": 99999, "Torque": 0, "Temp": 0, "Mode": 0, "Counter": 0})
    with pytest.raises(StestError, match="no message 'Nope'"):
        dbc.encode("Nope", {})
    with pytest.raises(StestError, match="Cmd|signal"):
        dbc.encode("Request", {})  # missing signal
    with pytest.raises(StestError, match="cannot load DBC"):
        Dbc(DBC_PATH.parent / "missing.dbc")


STATUS = {"Speed": 1000.0, "Torque": 5, "Temp": 25.0, "Mode": "Run", "Counter": 1}


@pytest.mark.parametrize("quantum_us", [1000, 37])
def test_send_periodic_exact_multiples(dbc, quantum_us):
    sim = Sim(quantum_us=quantum_us)
    bus = sim.can_bus("b", BR)
    ecu = bus.attach(DbcDevice("ecu", dbc))
    tester = bus.attach(DbcDevice("tester", dbc))
    ecu.send_periodic("Status", 50, STATUS)
    sim.start()
    sim.advance(ms=260)
    f = dbc.encode("Status", STATUS)
    assert [lf.t_ns for lf in bus.log] == [k * 50_000_000 + f.duration_ns(BR) for k in range(1, 6)]
    assert all(lf.sender == "ecu" for lf in bus.log)
    assert tester.received["Status"] == (bus.log[-1].t_ns, STATUS)
    assert "Status" not in ecu.received  # a device does not see its own frames
    sim.close()


@pytest.mark.parametrize("quantum_us", [1000, 37])
def test_send_periodic_callable_and_contention(dbc, quantum_us):
    sim = Sim(quantum_us=quantum_us)
    bus = sim.can_bus("b", BR)
    ecu = bus.attach(DbcDevice("ecu", dbc))
    other = bus.attach(DbcDevice("other", dbc))
    n = []

    def sig():
        n.append(1)
        return {**STATUS, "Counter": len(n)}

    ecu.send_periodic("Status", 50, sig)  # 0x100
    other.send_periodic("ExtInfo", 50, {"Level": 1, "Flags": 2})  # extended: loses to 0x100
    sim.start()
    sim.advance(ms=110)
    st = [lf for lf in bus.log if lf.frame.id == 0x100]
    ex = [lf for lf in bus.log if lf.frame.ext]
    assert [dbc.decode(lf.frame)[1]["Counter"] for lf in st] == [1, 2]
    d_st = dbc.encode("Status", STATUS).duration_ns(BR)
    assert [lf.t_ns for lf in st] == [50_000_000 + d_st, 100_000_000 + d_st]
    d_ex = dbc.encode("ExtInfo", {"Level": 1, "Flags": 2}).duration_ns(BR)
    assert [lf.t_ns for lf in ex] == [50_000_000 + d_st + IFS + d_ex, 100_000_000 + d_st + IFS + d_ex]
    assert list(ecu.received) == ["ExtInfo"] and other.received["Status"][1]["Counter"] == 2
    sim.close()


def test_send_message_and_decoded_log(dbc):
    sim = Sim()
    bus = sim.can_bus("b", BR)
    a = bus.attach(DbcDevice("a", dbc))
    b = bus.attach(DbcDevice("b", dbc))
    sim.start()
    a.send_message("Request", Cmd=9)
    a.send(CanFrame(0x7AA, b"\x01"))  # unknown to the DBC
    a.send_message("Reply", Echo=4)
    sim.advance(ms=2)
    msgs = bus.messages(dbc)
    # all three contend at t=0: arbitration by id (Reply 0x181 < Request 0x1F0 < 0x7AA)
    assert [(m, s) for _, m, s in msgs] == [("Reply", {"Echo": 4}), ("Request", {"Cmd": 9})]
    assert [lf.frame.id for lf in bus.log] == [0x181, 0x1F0, 0x7AA]
    assert msgs[1][0] == bus.log[1].t_ns
    assert b.received["Request"] == (bus.log[1].t_ns, {"Cmd": 9})
    with pytest.raises(StestError, match="no message"):
        a.send_periodic("Nope", 10, {})
    sim.close()


def test_wait_for_message_timeout_and_where(dbc):
    sim = Sim()
    bus = sim.can_bus("b", BR)
    a = bus.attach(DbcDevice("a", dbc))
    bus.attach(DbcDevice("b", dbc))
    cnt = []
    a.send_periodic("Reply", 5, lambda: cnt.append(1) or {"Echo": len(cnt)})
    sim.start()
    t, sig = bus.wait_for_message(dbc, "Reply", timeout_ms=100, where=lambda s: s["Echo"] == 3)
    assert sig == {"Echo": 3} and t == 15_000_000 + dbc.encode("Reply", {"Echo": 3}).duration_ns(BR)
    with pytest.raises(WaitTimeout):
        bus.wait_for_message(dbc, "Request", timeout_ms=20)
    sim.close()


@pytest.mark.parametrize("quantum_us", [1000, 37])
def test_firmware_echo_wait_for_message(can_exe, dbc, quantum_us):
    with Sim(quantum_us=quantum_us) as sim:
        bus = sim.can_bus("drive", BR)
        sim.node("n", can_exe(1), can={"CAN2": bus})
        tester = bus.attach(DbcDevice("tester", dbc))
        sim.start()
        sim.advance(ms=1)
        # the firmware answers Request (0x1F0) with Reply (0x181) echoing the command byte
        seq = iter(range(1, 100))
        tester.send_periodic("Request", 5, lambda: {"Cmd": next(seq)})
        t, sig = bus.wait_for_message(dbc, "Reply", timeout_ms=100, where=lambda s: s["Echo"] == 4)
        assert sig == {"Echo": 4}
        req = [(tt, s) for tt, m, s in bus.messages(dbc) if m == "Request"]
        assert [s["Cmd"] for _, s in req] == [1, 2, 3, 4]
        assert tester.received["Reply"] == (t, {"Echo": 4})
        # the periodic node frame 0x101 is not in the DBC and is skipped by the decoded log
        assert all(m in ("Request", "Reply") for _, m, _ in bus.messages(dbc))
        assert tester.received["Reply"][0] == t
        # exact: the 4th request goes out at 1 ms + 4 x 5 ms
        assert req[3][0] == 1_000_000 + 20_000_000 + dbc.encode("Request", {"Cmd": 4}).duration_ns(BR)


def test_low_id_extended_shares_id_with_standard(dbc):
    f = dbc.encode("LowExt", {"X": 5})
    assert f.ext and f.id == 0x100
    assert dbc.decode(f) == ("LowExt", {"X": 5})
    std = dbc.encode("Status", {"Speed": 0, "Torque": 0, "Temp": -40, "Mode": 0, "Counter": 0})
    assert not std.ext and dbc.decode(std)[0] == "Status"
    # a standard frame never matches the extended message, nor vice versa
    assert dbc.decode(CanFrame(0x101, b"\x05\x00")) is None
    assert dbc.decode(CanFrame(0x101, b"\x05\x00", ext=True)) is None


def test_wait_for_message_low_id_extended(dbc):
    sim = Sim()
    bus = sim.can_bus("b", BR)
    a = bus.attach(DbcDevice("a", dbc))
    b = bus.attach(DbcDevice("b", dbc))
    a.send_message("LowExt", X=77)
    sim.start()
    t, sig = bus.wait_for_message(dbc, "LowExt", timeout_ms=10)
    assert sig == {"X": 77}
    assert [m for _, m, _ in bus.messages(dbc)] == ["LowExt"]
    assert b.received["LowExt"] == (t, {"X": 77})
    sim.close()
