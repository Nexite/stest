"""The Quickstart test: stest's bundled blinky firmware, driven from pytest.

Run from the repository root after building stest (`cmake --build build`):
`uv run pytest examples/quickstart -q`. CI runs exactly that.
"""


# --8<-- [start:test]
def test_blinky_toggles_pa5(sim):
    node = sim.node("b", "build/selftest/blinky")
    sim.start()
    led = node.pin("PA5")
    led.wait_high(timeout_ms=600)  # virtual time; the host does not sleep
    sim.advance(ms=1600)
    assert [lvl for _, lvl in led.history] == [1, 0, 1, 0]
    assert node.var("uwTick") > 0  # firmware variables by name
# --8<-- [end:test]
