foreach(opt O0 O2)
  add_executable(mmio_order_${opt} mmio_order_test.c mmio_stubs.c mmio_mem_ir.ll)
  set_source_files_properties(mmio_mem_ir.ll PROPERTIES LANGUAGE C COMPILE_OPTIONS "-x;ir")
  target_compile_options(mmio_order_${opt} PRIVATE -${opt})
  stest_enable_mmio(mmio_order_${opt})
  add_test(NAME mmio_order_${opt} COMMAND mmio_order_${opt})
endforeach()

# Speed gate benchmarks (see run_gate.py, docs/spikes/2026-10-30-interception-gate.md).
# The MMIO model lives in its own library compiled WITHOUT the pass (it performs the
# real host volatile accesses itself).
add_library(bench_mmio_impl STATIC bench_mmio_impl.c)
target_compile_options(bench_mmio_impl PRIVATE -O2)
add_executable(bench_lighting_load bench_lighting_load.c)
target_compile_options(bench_lighting_load PRIVATE -O2)
stest_enable_mmio(bench_lighting_load)
target_link_libraries(bench_lighting_load PRIVATE bench_mmio_impl)
add_executable(bench_sync bench_sync.cpp)
target_compile_options(bench_sync PRIVATE -O2)
