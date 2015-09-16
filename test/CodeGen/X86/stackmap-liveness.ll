; RUN: llc < %s -mtriple=x86_64-apple-darwin -mcpu=corei7-avx -enable-patchpoint-liveness=false | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-apple-darwin -mcpu=corei7-avx                                   | FileCheck -check-prefix=PATCH %s
; RUN: llc < %s -mtriple=x86_64-apple-darwin -mcpu=corei7-avx -enable-patchpoint-liveness=false -stackmap-version=2 | FileCheck -check-prefix=V2CHECK %s
; RUN: llc < %s -mtriple=x86_64-apple-darwin -mcpu=corei7-avx -stackmap-version=2                                   | FileCheck -check-prefix=V2PATCH %s

;
; Note: Print verbose stackmaps using -debug-only=stackmaps.

; CHECK-LABEL:  .section  __LLVM_STACKMAPS,__llvm_stackmaps
; CHECK-NEXT:   __LLVM_StackMaps:
; Header
; CHECK-NEXT:   .byte 1
; CHECK-NEXT:   .byte 0
; CHECK-NEXT:   .short 0
; Num Functions
; CHECK-NEXT:   .long 2
; Num LargeConstants
; CHECK-NEXT:   .long   0
; Num Callsites
; CHECK-NEXT:   .long   5

; Functions and stack size
; CHECK-NEXT:   .quad _stackmap_liveness
; CHECK-NEXT:   .quad 8
; CHECK-NEXT:   .quad _mixed_liveness
; CHECK-NEXT:   .quad 8

define void @stackmap_liveness() {
entry:
  %a1 = call <2 x double> asm sideeffect "", "={xmm2}"() nounwind
; StackMap 1 (no liveness information available)
; CHECK-LABEL:  .long L{{.*}}-_stackmap_liveness
; CHECK-NEXT:   .short  0
; CHECK-NEXT:   .short  0
; Padding
; CHECK-NEXT:   .short  0
; Num LiveOut Entries: 0
; CHECK-NEXT:   .short  0
; Align
; CHECK-NEXT:   .align  3

; StackMap 1 (patchpoint liveness information enabled)
; PATCH-LABEL:  .long L{{.*}}-_stackmap_liveness
; PATCH-NEXT:   .short  0
; PATCH-NEXT:   .short  0
; Padding
; PATCH-NEXT:   .short  0
; Num LiveOut Entries: 1
; PATCH-NEXT:   .short  1
; LiveOut Entry 1: %YMM2 (16 bytes) --> %XMM2
; PATCH-NEXT:   .short  19
; PATCH-NEXT:   .byte 0
; PATCH-NEXT:   .byte 16
; Align
; PATCH-NEXT:   .align  3
  call anyregcc void (i64, i32, i8*, i32, ...) @llvm.experimental.patchpoint.void(i64 1, i32 12, i8* null, i32 0)
  %a2 = call i64 asm sideeffect "", "={r8}"() nounwind
  %a3 = call i8 asm sideeffect "", "={ah}"() nounwind
  %a4 = call <4 x double> asm sideeffect "", "={ymm0}"() nounwind
  %a5 = call <4 x double> asm sideeffect "", "={ymm1}"() nounwind

; StackMap 2 (no liveness information available)
; CHECK-LABEL:  .long L{{.*}}-_stackmap_liveness
; CHECK-NEXT:   .short  0
; CHECK-NEXT:   .short  0
; Padding
; CHECK-NEXT:   .short  0
; Num LiveOut Entries: 0
; CHECK-NEXT:   .short  0
; Align
; CHECK-NEXT:   .align  3

; StackMap 2 (patchpoint liveness information enabled)
; PATCH-LABEL:  .long L{{.*}}-_stackmap_liveness
; PATCH-NEXT:   .short  0
; PATCH-NEXT:   .short  0
; Padding
; PATCH-NEXT:   .short  0
; Num LiveOut Entries: 5
; PATCH-NEXT:   .short  5
; LiveOut Entry 1: %RAX (1 bytes) --> %AL or %AH
; PATCH-NEXT:   .short  0
; PATCH-NEXT:   .byte 0
; PATCH-NEXT:   .byte 1
; LiveOut Entry 2: %R8 (8 bytes)
; PATCH-NEXT:   .short  8
; PATCH-NEXT:   .byte 0
; PATCH-NEXT:   .byte 8
; LiveOut Entry 3: %YMM0 (32 bytes)
; PATCH-NEXT:   .short  17
; PATCH-NEXT:   .byte 0
; PATCH-NEXT:   .byte 32
; LiveOut Entry 4: %YMM1 (32 bytes)
; PATCH-NEXT:   .short  18
; PATCH-NEXT:   .byte 0
; PATCH-NEXT:   .byte 32
; LiveOut Entry 5: %YMM2 (16 bytes) --> %XMM2
; PATCH-NEXT:   .short  19
; PATCH-NEXT:   .byte 0
; PATCH-NEXT:   .byte 16
; Align
; PATCH-NEXT:   .align  3
  call anyregcc void (i64, i32, i8*, i32, ...) @llvm.experimental.patchpoint.void(i64 2, i32 12, i8* null, i32 0)
  call void asm sideeffect "", "{r8},{ah},{ymm0},{ymm1}"(i64 %a2, i8 %a3, <4 x double> %a4, <4 x double> %a5) nounwind

; StackMap 3 (no liveness information available)
; CHECK-LABEL:  .long L{{.*}}-_stackmap_liveness
; CHECK-NEXT:   .short  0
; CHECK-NEXT:   .short  0
; Padding
; CHECK-NEXT:   .short  0
; Num LiveOut Entries: 0
; CHECK-NEXT:   .short  0
; Align
; CHECK-NEXT:   .align  3

; StackMap 3 (patchpoint liveness information enabled)
; PATCH-LABEL:  .long L{{.*}}-_stackmap_liveness
; PATCH-NEXT:   .short  0
; PATCH-NEXT:   .short  0
; Padding
; PATCH-NEXT:   .short  0
; Num LiveOut Entries: 2
; PATCH-NEXT:   .short  2
; LiveOut Entry 1: %RSP (8 bytes)
; PATCH-NEXT:   .short  7
; PATCH-NEXT:   .byte 0
; PATCH-NEXT:   .byte 8
; LiveOut Entry 2: %YMM2 (16 bytes) --> %XMM2
; PATCH-NEXT:   .short  19
; PATCH-NEXT:   .byte 0
; PATCH-NEXT:   .byte 16
; Align
; PATCH-NEXT:   .align  3
  call anyregcc void (i64, i32, i8*, i32, ...) @llvm.experimental.patchpoint.void(i64 3, i32 12, i8* null, i32 0)
  call void asm sideeffect "", "{xmm2}"(<2 x double> %a1) nounwind
  ret void
}

define void @mixed_liveness() {
entry:
  %a1 = call <2 x double> asm sideeffect "", "={xmm2}"() nounwind
; StackMap 4 (patchpoint liveness information enabled)
; PATCH-LABEL:  .long L{{.*}}-_mixed_liveness
; PATCH-NEXT:   .short  0
; PATCH-NEXT:   .short  0
; Padding
; PATCH-NEXT:   .short  0
; Num LiveOut Entries: 0
; PATCH-NEXT:   .short  0
; Align
; PATCH-NEXT:   .align  3

; StackMap 5 (patchpoint liveness information enabled)
; PATCH-LABEL:  .long L{{.*}}-_mixed_liveness
; PATCH-NEXT:   .short  0
; PATCH-NEXT:   .short  0
; Padding
; PATCH-NEXT:   .short  0
; Num LiveOut Entries: 2
; PATCH-NEXT:   .short  2
; LiveOut Entry 1: %RSP (8 bytes)
; PATCH-NEXT:   .short  7
; PATCH-NEXT:   .byte 0
; PATCH-NEXT:   .byte 8
; LiveOut Entry 2: %YMM2 (16 bytes) --> %XMM2
; PATCH-NEXT:   .short  19
; PATCH-NEXT:   .byte 0
; PATCH-NEXT:   .byte 16
; Align
; PATCH-NEXT:   .align  3
  call void (i64, i32, ...) @llvm.experimental.stackmap(i64 4, i32 5)
  call anyregcc void (i64, i32, i8*, i32, ...) @llvm.experimental.patchpoint.void(i64 5, i32 0, i8* null, i32 0)
  call void asm sideeffect "", "{xmm2}"(<2 x double> %a1) nounwind
  ret void
}

declare void @llvm.experimental.stackmap(i64, i32, ...)
declare void @llvm.experimental.patchpoint.void(i64, i32, i8*, i32, ...)

; V2CHECK-LABEL:	.section	__LLVM_STACKMAPS,__llvm_stackmaps
; Header
; V2CHECK-NEXT:__LLVM_StackMaps:
; V2CHECK-NEXT:	.byte	2
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	2
; V2CHECK-NEXT:	.long	L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.long	L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.long	L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.long	L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.long	L{{.*}}-__LLVM_StackMaps

; Constant Array
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:L{{.*}}:

; FrameRecordMap Array
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:L{{.*}}:
; V2CHECK-NEXT:	.quad	_stackmap_liveness
; V2CHECK-NEXT:	.long	L{{.*}}-_stackmap_liveness
; V2CHECK-NEXT:	.long	8
; V2CHECK-NEXT:	.short	1
; V2CHECK-NEXT:	.short	6
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	3
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:	.quad	_mixed_liveness
; V2CHECK-NEXT:	.long	L{{.*}}-_mixed_liveness
; V2CHECK-NEXT:	.long	8
; V2CHECK-NEXT:	.short	1
; V2CHECK-NEXT:	.short	6
; V2CHECK-NEXT:	.short	3
; V2CHECK-NEXT:	.short	2
; V2CHECK-NEXT:	.align	3

; StackMapRecord Array
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:L{{.*}}:
; V2CHECK-NEXT:	.quad	1
; V2CHECK-NEXT:	.long	L{{.*}}-_stackmap_liveness
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:	.quad	2
; V2CHECK-NEXT:	.long	L{{.*}}-_stackmap_liveness
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:	.quad	3
; V2CHECK-NEXT:	.long	L{{.*}}-_stackmap_liveness
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:	.quad	4
; V2CHECK-NEXT:	.long	L{{.*}}-_mixed_liveness
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:	.quad	5
; V2CHECK-NEXT:	.long	L{{.*}}-_mixed_liveness
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.align	3

; V2PATCH-LABEL:	.section	__LLVM_STACKMAPS,__llvm_stackmaps
; V2PATCH-NEXT:__LLVM_StackMaps:
; Header
; V2PATCH-NEXT:	.byte	2
; V2PATCH-NEXT:	.byte	0
; V2PATCH-NEXT:	.short	0
; V2PATCH-NEXT:	.long	2
; V2PATCH-NEXT:	.long	L{{.*}}-__LLVM_StackMaps
; V2PATCH-NEXT:	.long	L{{.*}}-__LLVM_StackMaps
; V2PATCH-NEXT:	.long	L{{.*}}-__LLVM_StackMaps
; V2PATCH-NEXT:	.long	L{{.*}}-__LLVM_StackMaps
; V2PATCH-NEXT:	.long	L{{.*}}-__LLVM_StackMaps

; Constant Array
; V2PATCH-NEXT:	.align	3
; V2PATCH-NEXT:L{{.*}}:

; FrameRecordMap Array
; V2PATCH-NEXT:	.align	3
; V2PATCH-NEXT:L{{.*}}:
; V2PATCH-NEXT:	.quad	_stackmap_liveness
; V2PATCH-NEXT:	.long	L{{.*}}-_stackmap_liveness
; V2PATCH-NEXT:	.long	8
; V2PATCH-NEXT:	.short	9
; V2PATCH-NEXT:	.short	6
; V2PATCH-NEXT:	.short	0
; V2PATCH-NEXT:	.short	3
; V2PATCH-NEXT:	.align	3
; V2PATCH-NEXT:	.quad	_mixed_liveness
; V2PATCH-NEXT:	.long	L{{.*}}-_mixed_liveness
; V2PATCH-NEXT:	.long	8
; V2PATCH-NEXT:	.short	9
; V2PATCH-NEXT:	.short	6
; V2PATCH-NEXT:	.short	3
; V2PATCH-NEXT:	.short	2
; V2PATCH-NEXT:	.align	3

; StackMapRecord Array
; V2PATCH-NEXT:	.align	3
; V2PATCH-NEXT:L{{.*}}:
; V2PATCH-NEXT:	.quad	1
; V2PATCH-NEXT:	.long	L{{.*}}-_stackmap_liveness
; V2PATCH-NEXT:	.byte	0
; V2PATCH-NEXT:	.byte	1
; V2PATCH-NEXT:	.short	0
; V2PATCH-NEXT:	.short	0
; V2PATCH-NEXT:	.short	0
; V2PATCH-NEXT:	.short	1
; V2PATCH-NEXT:	.align	3
; V2PATCH-NEXT:	.quad	2
; V2PATCH-NEXT:	.long	L{{.*}}-_stackmap_liveness
; V2PATCH-NEXT:	.byte	0
; V2PATCH-NEXT:	.byte	1
; V2PATCH-NEXT:	.short	0
; V2PATCH-NEXT:	.short	0
; V2PATCH-NEXT:	.short	1
; V2PATCH-NEXT:	.short	5
; V2PATCH-NEXT:	.align	3
; V2PATCH-NEXT:	.quad	3
; V2PATCH-NEXT:	.long	L{{.*}}-_stackmap_liveness
; V2PATCH-NEXT:	.byte	0
; V2PATCH-NEXT:	.byte	1
; V2PATCH-NEXT:	.short	0
; V2PATCH-NEXT:	.short	0
; V2PATCH-NEXT:	.short	6
; V2PATCH-NEXT:	.short	2
; V2PATCH-NEXT:	.align	3
; V2PATCH-NEXT:	.quad	4
; V2PATCH-NEXT:	.long	L{{.*}}-_mixed_liveness
; V2PATCH-NEXT:	.byte	0
; V2PATCH-NEXT:	.byte	0
; V2PATCH-NEXT:	.short	0
; V2PATCH-NEXT:	.short	0
; V2PATCH-NEXT:	.short	8
; V2PATCH-NEXT:	.short	0
; V2PATCH-NEXT:	.align	3
; V2PATCH-NEXT:	.quad	5
; V2PATCH-NEXT:	.long	L{{.*}}-_mixed_liveness
; V2PATCH-NEXT:	.byte	0
; V2PATCH-NEXT:	.byte	1
; V2PATCH-NEXT:	.short	0
; V2PATCH-NEXT:	.short	0
; V2PATCH-NEXT:	.short	8
; V2PATCH-NEXT:	.short	2
; V2PATCH-NEXT:	.align	3

; Location Array
; V2PATCH-NEXT:	.align	2
; V2PATCH-NEXT:L{{.*}}:

; LiveOut Array
; V2PATCH-NEXT:	.align	1
; V2PATCH-NEXT:L{{.*}}:
; V2PATCH-NEXT:	.short	19
; V2PATCH-NEXT:	.byte	0
; V2PATCH-NEXT:	.byte	16
; V2PATCH-NEXT:	.align	1
; V2PATCH-NEXT:	.short	0
; V2PATCH-NEXT:	.byte	0
; V2PATCH-NEXT:	.byte	1
; V2PATCH-NEXT:	.align	1
; V2PATCH-NEXT:	.short	8
; V2PATCH-NEXT:	.byte	0
; V2PATCH-NEXT:	.byte	8
; V2PATCH-NEXT:	.align	1
; V2PATCH-NEXT:	.short	17
; V2PATCH-NEXT:	.byte	0
; V2PATCH-NEXT:	.byte	32
; V2PATCH-NEXT:	.align	1
; V2PATCH-NEXT:	.short	18
; V2PATCH-NEXT:	.byte	0
; V2PATCH-NEXT:	.byte	32
; V2PATCH-NEXT:	.align	1
; V2PATCH-NEXT:	.short	19
; V2PATCH-NEXT:	.byte	0
; V2PATCH-NEXT:	.byte	16
; V2PATCH-NEXT:	.align	1
; V2PATCH-NEXT:	.short	7
; V2PATCH-NEXT:	.byte	0
; V2PATCH-NEXT:	.byte	8
; V2PATCH-NEXT:	.align	1
; V2PATCH-NEXT:	.short	19
; V2PATCH-NEXT:	.byte	0
; V2PATCH-NEXT:	.byte	16
; V2PATCH-NEXT:	.align	1
; V2PATCH-NEXT:	.short	7
; V2PATCH-NEXT:	.byte	0
; V2PATCH-NEXT:	.byte	8
; V2PATCH-NEXT:	.align	1
; V2PATCH-NEXT:	.short	19
; V2PATCH-NEXT:	.byte	0
; V2PATCH-NEXT:	.byte	16
; V2PATCH-NEXT:	.align	1


