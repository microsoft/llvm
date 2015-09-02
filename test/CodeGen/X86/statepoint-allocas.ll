; RUN: llc < %s | FileCheck %s
; RUN: llc -stackmap-version=2 < %s | FileCheck --check-prefix=V2CHECK %s 
; Check that we can lower a use of an alloca both as a deopt value (where the
; exact meaning is up to the consumer of the stackmap) and as an explicit spill
; slot used for GC.  

target datalayout = "e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

declare zeroext i1 @return_i1()

; Can we handle an explicit relocation slot (in the form of an alloca) given 
; to the statepoint?
define i32 addrspace(1)* @test(i32 addrspace(1)* %ptr) gc "statepoint-example" {
; CHECK-LABEL: test
; CHECK: pushq  %rax
; CHECK: movq   %rdi, (%rsp)
; CHECK: callq return_i1
; CHECK: movq   (%rsp), %rax
; CHECK: popq   %rdx
; CHECK: retq
entry:
  %alloca = alloca i32 addrspace(1)*, align 8
  store i32 addrspace(1)* %ptr, i32 addrspace(1)** %alloca
  call i32 (i64, i32, i1 ()*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_i1f(i64 0, i32 0, i1 ()* @return_i1, i32 0, i32 0, i32 0, i32 0, i32 addrspace(1)** %alloca)
  %rel = load i32 addrspace(1)*, i32 addrspace(1)** %alloca
  ret i32 addrspace(1)* %rel
}

; Can we handle an alloca as a deopt value?  
define i32 addrspace(1)* @test2(i32 addrspace(1)* %ptr) gc "statepoint-example" {
; CHECK-LABEL: test2
; CHECK: pushq  %rax
; CHECK: movq   %rdi, (%rsp)
; CHECK: callq return_i1
; CHECK: xorl   %eax, %eax
; CHECK: popq   %rdx
; CHECK: retq
entry:
  %alloca = alloca i32 addrspace(1)*, align 8
  store i32 addrspace(1)* %ptr, i32 addrspace(1)** %alloca
  call i32 (i64, i32, i1 ()*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_i1f(i64 0, i32 0, i1 ()* @return_i1, i32 0, i32 0, i32 0, i32 1, i32 addrspace(1)** %alloca)
  ret i32 addrspace(1)* null
}

declare i32 @llvm.experimental.gc.statepoint.p0f_i1f(i64, i32, i1 ()*, i32, i32, ...)


; CHECK-LABEL: .section .llvm_stackmaps
; CHECK-NEXT:  __LLVM_StackMaps:
; Header
; CHECK-NEXT:   .byte 1
; CHECK-NEXT:   .byte 0
; CHECK-NEXT:   .short 0
; Num Functions
; CHECK-NEXT:   .long 2
; Num LargeConstants
; CHECK-NEXT:   .long 0
; Num Callsites
; CHECK-NEXT:   .long 2

; Functions and stack size
; CHECK-NEXT:   .quad test
; CHECK-NEXT:   .quad 8
; CHECK-NEXT:   .quad test2
; CHECK-NEXT:   .quad 8

; Large Constants
; Statepoint ID only
; CHECK: .quad	0

; Callsites
; The GC one
; CHECK: .long	.L{{.*}}-test
; CHECK: .short	0
; CHECK: .short	4
; SmallConstant (0)
; CHECK: .byte	4
; CHECK: .byte	8
; CHECK: .short	0
; CHECK: .long	0
; SmallConstant (0)
; CHECK: .byte	4
; CHECK: .byte	8
; CHECK: .short	0
; CHECK: .long	0
; SmallConstant (0)
; CHECK: .byte	4
; CHECK: .byte	8
; CHECK: .short	0
; CHECK: .long	0
; Direct Spill Slot [RSP+0]
; CHECK: .byte	2
; CHECK: .byte	8
; CHECK: .short	7
; CHECK: .long	0
; No Padding or LiveOuts
; CHECK: .short	0
; CHECK: .short	0
; CHECK: .align	8

; The Deopt one
; CHECK: .long	.L{{.*}}-test2
; CHECK: .short	0
; CHECK: .short	4
; SmallConstant (0)
; CHECK: .byte	4
; CHECK: .byte	8
; CHECK: .short	0
; CHECK: .long	0
; SmallConstant (0)
; CHECK: .byte	4
; CHECK: .byte	8
; CHECK: .short	0
; CHECK: .long	0
; SmallConstant (1)
; CHECK: .byte	4
; CHECK: .byte	8
; CHECK: .short	0
; CHECK: .long	1
; Direct Spill Slot [RSP+0]
; CHECK: .byte	2
; CHECK: .byte	8
; CHECK: .short	7
; CHECK: .long	0

; No Padding or LiveOuts
; CHECK: .short	0
; CHECK: .short	0
; CHECK: .align	8

; V2CHECK-LABEL:	.section	.llvm_stackmaps
; V2CHECK-NEXT: __LLVM_StackMaps:
; Header
; V2CHECK-NEXT:	.byte	2
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	2
; V2CHECK-NEXT:	.long	.L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.long	.L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.long	.L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.long	.L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.long	.L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.align	8

; Constant Array
; V2CHECK-NEXT:.L{{.*}}:
; V2CHECK-NEXT:	.align	8

; FrameRecordMap Array
; V2CHECK-NEXT:.L{{.*}}:
; V2CHECK-NEXT:	.quad	test
; V2CHECK-NEXT:	.long	.L{{.*}}-test
; V2CHECK-NEXT:	.long	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	7
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	1
; V2CHECK-NEXT:	.align	8
; V2CHECK-NEXT:	.quad	test2
; V2CHECK-NEXT:	.long	.L{{.*}}-test2
; V2CHECK-NEXT:	.long	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	7
; V2CHECK-NEXT:	.short	1
; V2CHECK-NEXT:	.short	1
; V2CHECK-NEXT:	.align	8
; V2CHECK-NEXT:	.align	8

; StackMapRecord Array
; V2CHECK-NEXT:.L{{.*}}:
; V2CHECK-NEXT:	.quad	0
; V2CHECK-NEXT:	.long	.L{{.*}}-test
; V2CHECK-NEXT:	.byte	.L{{.*}}-.L{{.*}}
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	4
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.align	8
; V2CHECK-NEXT:	.quad	0
; V2CHECK-NEXT:	.long	.L{{.*}}-test2
; V2CHECK-NEXT:	.byte	.L{{.*}}-.L{{.*}}
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.short	4
; V2CHECK-NEXT:	.short	4
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.align	8
; V2CHECK-NEXT:	.align	4

; Location Array
; V2CHECK-NEXT:.L{{.*}}:
; V2CHECK-NEXT:	.byte	4
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	0
; V2CHECK-NEXT:	.align	4
; V2CHECK-NEXT:	.byte	4
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	0
; V2CHECK-NEXT:	.align	4
; V2CHECK-NEXT:	.byte	4
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	0
; V2CHECK-NEXT:	.align	4
; V2CHECK-NEXT:	.byte	2
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	7
; V2CHECK-NEXT:	.long	0
; V2CHECK-NEXT:	.align	4
; V2CHECK-NEXT:	.byte	4
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	0
; V2CHECK-NEXT:	.align	4
; V2CHECK-NEXT:	.byte	4
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	0
; V2CHECK-NEXT:	.align	4
; V2CHECK-NEXT:	.byte	4
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	1
; V2CHECK-NEXT:	.align	4
; V2CHECK-NEXT:	.byte	2
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	7
; V2CHECK-NEXT:	.long	0
; V2CHECK-NEXT:	.align	4
; V2CHECK-NEXT:	.align	2

