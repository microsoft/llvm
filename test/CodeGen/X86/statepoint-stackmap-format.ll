; RUN: llc < %s -mtriple="x86_64-pc-linux-gnu" | FileCheck %s
; RUN: llc < %s -mtriple="x86_64-pc-win64-coff" | FileCheck %s
; RUN: llc < %s -mtriple="x86_64-pc-linux-gnu" -stackmap-version=2 | FileCheck --check-prefix=V2CHECK %s
; RUN: llc < %s -mtriple="x86_64-pc-win64-coff" -stackmap-version=2 | FileCheck --check-prefix=V2CHECK %s

; This test is a sanity check to ensure statepoints are generating StackMap
; sections correctly.  This is not intended to be a rigorous test of the 
; StackMap format (see the stackmap tests for that).

target datalayout = "e-i64:64-f80:128-n8:16:32:64-S128"

declare zeroext i1 @return_i1()

define i1 @test(i32 addrspace(1)* %ptr_base, i32 %arg)
  gc "statepoint-example" {
; CHECK-LABEL: test
; Do we see two spills for the local values and the store to the
; alloca?
; CHECK: subq	$40, %rsp
; CHECK: movq	$0,   24(%rsp)
; CHECK: movq	%rdi, 16(%rsp)
; CHECK: movq	%rax, 8(%rsp)
; CHECK: callq return_i1
; CHECK: addq	$40, %rsp
; CHECK: retq
entry:
  %metadata1 = alloca i32 addrspace(1)*, i32 2, align 8
  store i32 addrspace(1)* null, i32 addrspace(1)** %metadata1
  %ptr_derived = getelementptr i32, i32 addrspace(1)* %ptr_base, i32 %arg
  %safepoint_token = tail call i32 (i64, i32, i1 ()*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_i1f(i64 0, i32 0, i1 ()* @return_i1, i32 0, i32 0, i32 0, i32 2, i32 addrspace(1)* %ptr_base, i32 addrspace(1)* null, i32 addrspace(1)* %ptr_base, i32 addrspace(1)* %ptr_derived, i32 addrspace(1)* null)
  %call1 = call zeroext i1 @llvm.experimental.gc.result.i1(i32 %safepoint_token)
  %a = call i32 addrspace(1)* @llvm.experimental.gc.relocate.p1i32(i32 %safepoint_token, i32 9, i32 9)
  %b = call i32 addrspace(1)* @llvm.experimental.gc.relocate.p1i32(i32 %safepoint_token, i32 9, i32 10)
  %c = call i32 addrspace(1)* @llvm.experimental.gc.relocate.p1i32(i32 %safepoint_token, i32 11, i32 11)
; 
  ret i1 %call1
}

; This is similar to the previous test except that we have derived pointer as
; argument to the function. Despite that this can not happen after the
; RewriteSafepointForGC pass, lowering should be able to handle it anyway.
define i1 @test_derived_arg(i32 addrspace(1)* %ptr_base,
                            i32 addrspace(1)* %ptr_derived)
  gc "statepoint-example" {
; CHECK-LABEL: test_derived_arg
; Do we see two spills for the local values and the store to the
; alloca?
; CHECK: subq	$40, %rsp
; CHECK: movq	$0,   24(%rsp)
; CHECK: movq	%rdi, 16(%rsp)
; CHECK: movq	%rsi, 8(%rsp)
; CHECK: callq return_i1
; CHECK: addq	$40, %rsp
; CHECK: retq
entry:
  %metadata1 = alloca i32 addrspace(1)*, i32 2, align 8
  store i32 addrspace(1)* null, i32 addrspace(1)** %metadata1
  %safepoint_token = tail call i32 (i64, i32, i1 ()*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_i1f(i64 0, i32 0, i1 ()* @return_i1, i32 0, i32 0, i32 0, i32 2, i32 addrspace(1)* %ptr_base, i32 addrspace(1)* null, i32 addrspace(1)* %ptr_base, i32 addrspace(1)* %ptr_derived, i32 addrspace(1)* null)
  %call1 = call zeroext i1 @llvm.experimental.gc.result.i1(i32 %safepoint_token)
  %a = call i32 addrspace(1)* @llvm.experimental.gc.relocate.p1i32(i32 %safepoint_token, i32 9, i32 9)
  %b = call i32 addrspace(1)* @llvm.experimental.gc.relocate.p1i32(i32 %safepoint_token, i32 9, i32 10)
  %c = call i32 addrspace(1)* @llvm.experimental.gc.relocate.p1i32(i32 %safepoint_token, i32 11, i32 11)
; 
  ret i1 %call1
}

; Simple test case to check that we emit the ID field correctly
define i1 @test_id() gc "statepoint-example" {
; CHECK-LABEL: test_id
entry:
  %safepoint_token = tail call i32 (i64, i32, i1 ()*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_i1f(i64 237, i32 0, i1 ()* @return_i1, i32 0, i32 0, i32 0, i32 0)
  %call1 = call zeroext i1 @llvm.experimental.gc.result.i1(i32 %safepoint_token)
  ret i1 %call1
}


declare i32 @llvm.experimental.gc.statepoint.p0f_i1f(i64, i32, i1 ()*, i32, i32, ...)
declare i1 @llvm.experimental.gc.result.i1(i32)
declare i32 addrspace(1)* @llvm.experimental.gc.relocate.p1i32(i32, i32, i32) #3

; CHECK-LABEL: .section .llvm_stackmaps
; CHECK-NEXT:  __LLVM_StackMaps:
; Header
; CHECK-NEXT:   .byte 1
; CHECK-NEXT:   .byte 0
; CHECK-NEXT:   .short 0
; Num Functions
; CHECK-NEXT:   .long 3
; Num LargeConstants
; CHECK-NEXT:   .long 0
; Num Callsites
; CHECK-NEXT:   .long 3

; Functions and stack size
; CHECK-NEXT:   .quad test
; CHECK-NEXT:   .quad 40
; CHECK-NEXT:   .quad test_derived_arg
; CHECK-NEXT:   .quad 40

;
; test
;

; Large Constants
; Statepoint ID only
; CHECK: .quad	0

; Callsites
; Constant arguments
; CHECK: .long	.L{{.*}}-test
; CHECK: .short	0
; CHECK: .short	11
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
; SmallConstant (2)
; CHECK: .byte	4
; CHECK: .byte	8
; CHECK: .short	0
; CHECK: .long	2
; Direct Spill Slot [RSP+0]
; CHECK: .byte	2
; CHECK: .byte	8
; CHECK: .short	7
; CHECK: .long	16
; SmallConstant  (0)
; CHECK: .byte	4
; CHECK: .byte	8
; CHECK: .short	0
; CHECK: .long	0
; SmallConstant  (0)
; CHECK: .byte	4
; CHECK: .byte	8
; CHECK: .short	0
; CHECK: .long	0
; SmallConstant  (0)
; CHECK: .byte	4
; CHECK: .byte	8
; CHECK: .short	0
; CHECK: .long	0
; Direct Spill Slot [RSP+16]
; CHECK: .byte	2
; CHECK: .byte	8
; CHECK: .short	7
; CHECK: .long	16
; Direct Spill Slot [RSP+8]
; CHECK: .byte	2
; CHECK: .byte	8
; CHECK: .short	7
; CHECK: .long	8
; Direct Spill Slot [RSP+16]
; CHECK: .byte	2
; CHECK: .byte	8
; CHECK: .short	7
; CHECK: .long	16
; Direct Spill Slot [RSP+16]
; CHECK: .byte	2
; CHECK: .byte	8
; CHECK: .short	7
; CHECK: .long	16

; No Padding or LiveOuts
; CHECK: .short	0
; CHECK: .short	0
; CHECK: .align	8

;
; test_derived_arg
;

; Large Constants
; Statepoint ID only
; CHECK: .quad	0

; Callsites
; Constant arguments
; CHECK: .long	.L{{.*}}-test_derived_arg
; CHECK: .short	0
; CHECK: .short	11
; SmallConstant (0)
; CHECK: .byte	4
; CHECK: .byte	8
; CHECK: .short	0
; CHECK: .long	0
; SmallConstant (2)
; CHECK: .byte	4
; CHECK: .byte	8
; CHECK: .short	0
; CHECK: .long	2
; Direct Spill Slot [RSP+0]
; CHECK: .byte	2
; CHECK: .byte	8
; CHECK: .short	7
; CHECK: .long	16
; SmallConstant  (0)
; CHECK: .byte	4
; CHECK: .byte	8
; CHECK: .short	0
; CHECK: .long	0
; SmallConstant  (0)
; CHECK: .byte	4
; CHECK: .byte	8
; CHECK: .short	0
; CHECK: .long	0
; SmallConstant  (0)
; CHECK: .byte	4
; CHECK: .byte	8
; CHECK: .short	0
; CHECK: .long	0
; Direct Spill Slot [RSP+16]
; CHECK: .byte	2
; CHECK: .byte	8
; CHECK: .short	7
; CHECK: .long	16
; Direct Spill Slot [RSP+8]
; CHECK: .byte	2
; CHECK: .byte	8
; CHECK: .short	7
; CHECK: .long	8
; Direct Spill Slot [RSP+16]
; CHECK: .byte	2
; CHECK: .byte	8
; CHECK: .short	7
; CHECK: .long	16
; Direct Spill Slot [RSP+16]
; CHECK: .byte	2
; CHECK: .byte	8
; CHECK: .short	7
; CHECK: .long	16

; No Padding or LiveOuts
; CHECK: .short	0
; CHECK: .short	0
; CHECK: .align	8

; Records for the test_id function:
; No large constants

; The Statepoint ID:
; CHECK: .quad	237

; Instruction Offset
; CHECK: .long	.L{{.*}}-test_id

; Reserved:
; CHECK: .short	0

; NumLocations:
; CHECK: .short	3

; StkMapRecord[0]:
; SmallConstant(0):
; CHECK: .byte	4
; CHECK: .byte	8
; CHECK: .short	0
; CHECK: .long	0

; StkMapRecord[1]:
; SmallConstant(0):
; CHECK: .byte	4
; CHECK: .byte	8
; CHECK: .short	0
; CHECK: .long	0

; StkMapRecord[2]:
; SmallConstant(0):
; CHECK: .byte	4
; CHECK: .byte	8
; CHECK: .short	0
; CHECK: .long	0

; No padding or LiveOuts
; CHECK: .short	0
; CHECK: .short	0
; CHECK: .align	8

; V2CHECK-LABEL:	.section	.llvm_stackmaps
; V2CHECK-NEXT:__LLVM_StackMaps:
; Header
; V2CHECK-NEXT:	.byte	2
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	3
; V2CHECK-NEXT:	.long	.L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.long	.L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.long	.L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.long	.L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.long	.L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.align	8

; Constant Array
; V2CHECK-NEXT:.L{{.*}}
; V2CHECK-NEXT:	.align	8

; FrameRecordMap Array
; V2CHECK-NEXT:.L{{.*}}
; V2CHECK-NEXT:	.quad	test
; V2CHECK-NEXT:	.long	.L{{.*}}-test
; V2CHECK-NEXT:	.long	40
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	7
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	1
; V2CHECK-NEXT:	.align	8
; V2CHECK-NEXT:	.quad	test_derived_arg
; V2CHECK-NEXT:	.long	.L{{.*}}-test_derived_arg
; V2CHECK-NEXT:	.long	40
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	7
; V2CHECK-NEXT:	.short	1
; V2CHECK-NEXT:	.short	1
; V2CHECK-NEXT:	.align	8
; V2CHECK-NEXT:	.quad	test_id
; V2CHECK-NEXT:	.long	.L{{.*}}-test_id
; V2CHECK-NEXT:	.long	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	7
; V2CHECK-NEXT:	.short	2
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
; V2CHECK-NEXT:	.short	11
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.align	8
; V2CHECK-NEXT:	.quad	0
; V2CHECK-NEXT:	.long	.L{{.*}}-test_derived_arg
; V2CHECK-NEXT:	.byte	.L{{.*}}-.L{{.*}}
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.short	11
; V2CHECK-NEXT:	.short	11
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.align	8
; V2CHECK-NEXT:	.quad	237
; V2CHECK-NEXT:	.long	.L{{.*}}-test_id
; V2CHECK-NEXT:	.byte	.L{{.*}}-.L{{.*}}
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.short	22
; V2CHECK-NEXT:	.short	3
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
; V2CHECK-NEXT:	.long	2
; V2CHECK-NEXT:	.align	4
; V2CHECK-NEXT:	.byte	2
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	7
; V2CHECK-NEXT:	.long	16
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
; V2CHECK-NEXT:	.long	0
; V2CHECK-NEXT:	.align	4
; V2CHECK-NEXT:	.byte	2
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	7
; V2CHECK-NEXT:	.long	16
; V2CHECK-NEXT:	.align	4
; V2CHECK-NEXT:	.byte	2
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	7
; V2CHECK-NEXT:	.long	8
; V2CHECK-NEXT:	.align	4
; V2CHECK-NEXT:	.byte	2
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	7
; V2CHECK-NEXT:	.long	16
; V2CHECK-NEXT:	.align	4
; V2CHECK-NEXT:	.byte	2
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	7
; V2CHECK-NEXT:	.long	16
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
; V2CHECK-NEXT:	.long	2
; V2CHECK-NEXT:	.align	4
; V2CHECK-NEXT:	.byte	2
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	7
; V2CHECK-NEXT:	.long	16
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
; V2CHECK-NEXT:	.long	0
; V2CHECK-NEXT:	.align	4
; V2CHECK-NEXT:	.byte	2
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	7
; V2CHECK-NEXT:	.long	16
; V2CHECK-NEXT:	.align	4
; V2CHECK-NEXT:	.byte	2
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	7
; V2CHECK-NEXT:	.long	8
; V2CHECK-NEXT:	.align	4
; V2CHECK-NEXT:	.byte	2
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	7
; V2CHECK-NEXT:	.long	16
; V2CHECK-NEXT:	.align	4
; V2CHECK-NEXT:	.byte	2
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	7
; V2CHECK-NEXT:	.long	16
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
; V2CHECK-NEXT:	.long	0
; V2CHECK-NEXT:	.align	4
; V2CHECK-NEXT:	.align	2
