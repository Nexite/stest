; Hand-written IR: clang cannot be made to emit volatile memset/memmove or a
; variable-length volatile memcpy from C, so these pin the pass's rewrites directly.
; Compiled with the pass plugin at -O0 and -O2 (see spike/CMakeLists.txt).
declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)
declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

define void @ir_memset7(ptr %d) {
  call void @llvm.memset.p0.i64(ptr %d, i8 90, i64 7, i1 true)
  ret void
}
define void @ir_memset_var(ptr %d, i8 %c, i64 %n) {
  call void @llvm.memset.p0.i64(ptr %d, i8 %c, i64 %n, i1 true)
  ret void
}
define void @ir_memmove12(ptr %d, ptr %s) {
  call void @llvm.memmove.p0.p0.i64(ptr %d, ptr %s, i64 12, i1 true)
  ret void
}
define void @ir_memcpy_var(ptr %d, ptr %s, i64 %n) {
  call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %s, i64 %n, i1 true)
  ret void
}
define void @ir_memset_nonvolatile(ptr %d) {
  call void @llvm.memset.p0.i64(ptr %d, i8 1, i64 4, i1 false)
  ret void
}
