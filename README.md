# stest

stest is an open-source (Apache-2.0) software-in-the-loop simulator for STM32 firmware. Firmware is compiled natively with upstream LLVM clang plus the stest pass plugin, which rewrites volatile (memory-mapped I/O) accesses into calls to a simulated peripheral runtime, so unmodified firmware sources can run and be tested on a host machine in virtual time (nanoseconds). Requires LLVM 18 or newer; Apple clang is not supported.
