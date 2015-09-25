; RUN: opt %s -rewrite-statepoints-for-gc -spp-print-base-pointers -S 2>&1 | FileCheck %s

@M = external global i32
@N = external global i64

declare i32 @process(i32 addrspace(1)*)
declare i32 @llvm.experimental.gc.statepoint.p0f_i32p1i32f(i64, i32, i32 (i32 addrspace(1)*)*, i32, i32, ...)
declare i32 @llvm.experimental.gc.result.i32(i32) #0

define i32 @global() gc "coreclr" {
entry:
  %global_ptr = addrspacecast i32* @M to i32 addrspace(1)*
  %safepoint_token = call i32 (i64, i32, i32 (i32 addrspace(1)*)*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_i32p1i32f(i64 1, i32 0, i32 (i32 addrspace(1)*)* @process, i32 1, i32 0, i32 addrspace(1)* %global_ptr, i32 0, i32 0)
  %res = call i32 @llvm.experimental.gc.result.i32(i32 %safepoint_token)
  ret i32 %res
}

define i32 @local() gc "coreclr" {
entry:
  %loc = alloca i32
  %local_ptr = addrspacecast i32* %loc to i32 addrspace(1)*
  %safepoint_token = call i32 (i64, i32, i32 (i32 addrspace(1)*)*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_i32p1i32f(i64 1, i32 0, i32 (i32 addrspace(1)*)* @process, i32 1, i32 0, i32 addrspace(1)* %local_ptr, i32 0, i32 0)
  %res = call i32 @llvm.experimental.gc.result.i32(i32 %safepoint_token)
  ret i32 %res
}

define i32 @heap(i32 addrspace(1)** %param0) gc "coreclr" {
entry:
  %heap_ptr = load i32 addrspace(1)*, i32 addrspace(1)** %param0, align 8
  %safepoint_token = call i32 (i64, i32, i32 (i32 addrspace(1)*)*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_i32p1i32f(i64 1, i32 0, i32 (i32 addrspace(1)*)* @process, i32 1, i32 0, i32 addrspace(1)* %heap_ptr, i32 0, i32 0)
  %res = call i32 @llvm.experimental.gc.result.i32(i32 %safepoint_token)
  ret i32 %res
}

define i32 @gcPtrCast(i64 addrspace(1)** %param0) gc "coreclr" {
  %param_ptr = load i64 addrspace(1)*, i64 addrspace(1)** %param0, align 8
  %cast_ptr = bitcast i64 addrspace(1)* %param_ptr to i32 addrspace(1)*
  %safepoint_token = call i32 (i64, i32, i32 (i32 addrspace(1)*)*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_i32p1i32f(i64 1, i32 0, i32 (i32 addrspace(1)*)* @process, i32 1, i32 0, i32 addrspace(1)* %cast_ptr, i32 0, i32 0)
  %res = call i32 @llvm.experimental.gc.result.i32(i32 %safepoint_token)
  ret i32 %res
}

define i32 @nativePtrCast(i32** %param0) gc "coreclr" {
  %param_ptr = load i32*, i32** %param0, align 8
  %cast_ptr = addrspacecast i32* %param_ptr to i32 addrspace(1)*
  %safepoint_token = call i32 (i64, i32, i32 (i32 addrspace(1)*)*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_i32p1i32f(i64 1, i32 0, i32 (i32 addrspace(1)*)* @process, i32 1, i32 0, i32 addrspace(1)* %cast_ptr, i32 0, i32 0)
  %res = call i32 @llvm.experimental.gc.result.i32(i32 %safepoint_token)
  ret i32 %res
}

define i32 @intCast() gc "coreclr" {
  %cast_ptr = inttoptr i64 12345678 to i32 addrspace(1)*
  %safepoint_token = call i32 (i64, i32, i32 (i32 addrspace(1)*)*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_i32p1i32f(i64 1, i32 0, i32 (i32 addrspace(1)*)* @process, i32 1, i32 0, i32 addrspace(1)* %cast_ptr, i32 0, i32 0)
  %res = call i32 @llvm.experimental.gc.result.i32(i32 %safepoint_token)
  ret i32 %res
}

define i32 @localorheap(i32 addrspace(1)** %param0, i1 %cond) gc "coreclr" {
  %loc = alloca i32  
  br i1 %cond, label %lptr, label %hptr
  
lptr:
  %l_ptr = addrspacecast i32* %loc to i32 addrspace(1)*
  br label %join

hptr:
  %h_ptr = load i32 addrspace(1)*, i32 addrspace(1)** %param0, align 8
  br label %join

join:
  %ptr = phi i32 addrspace(1)* [%l_ptr, %lptr], [%h_ptr, %hptr] 
  %safepoint_token = call i32 (i64, i32, i32 (i32 addrspace(1)*)*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_i32p1i32f(i64 1, i32 0, i32 (i32 addrspace(1)*)* @process, i32 1, i32 0, i32 addrspace(1)* %ptr, i32 0, i32 0)
  %res = call i32 @llvm.experimental.gc.result.i32(i32 %safepoint_token)
  ret i32 %res
}

define i32 @nullptr(i1 %cond) gc "coreclr" {
  br i1 %cond, label %true, label %false
  
true:
  br label %join

false:
  br label %join

join:
  %ptr = phi i32 addrspace(1)* [null, %true], [null, %false] 
  %safepoint_token = call i32 (i64, i32, i32 (i32 addrspace(1)*)*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_i32p1i32f(i64 1, i32 0, i32 (i32 addrspace(1)*)* @process, i32 1, i32 0, i32 addrspace(1)* %ptr, i32 0, i32 0)
  %res = call i32 @llvm.experimental.gc.result.i32(i32 %safepoint_token)
  ret i32 %res
}


