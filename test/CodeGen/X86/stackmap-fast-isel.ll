; RUN: llc < %s -mtriple=x86_64-apple-darwin -mcpu=corei7                             | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-apple-darwin -mcpu=corei7 -fast-isel -fast-isel-abort=1 | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-apple-darwin -mcpu=corei7 -stackmap-version=2 | FileCheck %s --check-prefix=V2CHECK
; RUN: llc < %s -mtriple=x86_64-apple-darwin -mcpu=corei7 -fast-isel -fast-isel-abort=1 -stackmap-version=2 | FileCheck %s --check-prefix=V2CHECK

; CHECK-LABEL:  .section  __LLVM_STACKMAPS,__llvm_stackmaps
; CHECK-NEXT:  __LLVM_StackMaps:
; Header
; CHECK-NEXT:   .byte 1
; CHECK-NEXT:   .byte 0
; CHECK-NEXT:   .short 0
; Num Functions
; CHECK-NEXT:   .long 4
; Num LargeConstants
; CHECK-NEXT:   .long 3
; Num Callsites
; CHECK-NEXT:   .long 7

; Functions and stack size
; CHECK-NEXT:   .quad _constantargs
; CHECK-NEXT:   .quad 8
; CHECK-NEXT:   .quad _liveConstant
; CHECK-NEXT:   .quad 8
; CHECK-NEXT:   .quad _directFrameIdx
; CHECK-NEXT:   .quad 40
; CHECK-NEXT:   .quad _longid
; CHECK-NEXT:   .quad 8

; Large Constants
; CHECK-NEXT:   .quad   2147483648
; CHECK-NEXT:   .quad   4294967295
; CHECK-NEXT:   .quad   4294967296

; Callsites
; Constant arguments
;
; CHECK-NEXT:   .quad   1
; CHECK-NEXT:   .long   L{{.*}}-_constantargs
; CHECK-NEXT:   .short  0
; CHECK-NEXT:   .short  12
; SmallConstant
; CHECK-NEXT:   .byte   4
; CHECK-NEXT:   .byte   8
; CHECK-NEXT:   .short  0
; CHECK-NEXT:   .long   -1
; SmallConstant
; CHECK-NEXT:   .byte   4
; CHECK-NEXT:   .byte   8
; CHECK-NEXT:   .short  0
; CHECK-NEXT:   .long   -1
; SmallConstant
; CHECK-NEXT:   .byte   4
; CHECK-NEXT:   .byte   8
; CHECK-NEXT:   .short  0
; CHECK-NEXT:   .long   65536
; SmallConstant
; CHECK-NEXT:   .byte   4
; CHECK-NEXT:   .byte   8
; CHECK-NEXT:   .short  0
; CHECK-NEXT:   .long   2000000000
; SmallConstant
; CHECK-NEXT:   .byte   4
; CHECK-NEXT:   .byte   8
; CHECK-NEXT:   .short  0
; CHECK-NEXT:   .long   2147483647
; SmallConstant
; CHECK-NEXT:   .byte   4
; CHECK-NEXT:   .byte   8
; CHECK-NEXT:   .short  0
; CHECK-NEXT:   .long   -1
; SmallConstant
; CHECK-NEXT:   .byte   4
; CHECK-NEXT:   .byte   8
; CHECK-NEXT:   .short  0
; CHECK-NEXT:   .long   -1
; SmallConstant
; CHECK-NEXT:   .byte   4
; CHECK-NEXT:   .byte   8
; CHECK-NEXT:   .short  0
; CHECK-NEXT:   .long   0
; LargeConstant at index 0
; CHECK-NEXT:   .byte   5
; CHECK-NEXT:   .byte   8
; CHECK-NEXT:   .short  0
; CHECK-NEXT:   .long   0
; LargeConstant at index 1
; CHECK-NEXT:   .byte   5
; CHECK-NEXT:   .byte   8
; CHECK-NEXT:   .short  0
; CHECK-NEXT:   .long   1
; LargeConstant at index 2
; CHECK-NEXT:   .byte   5
; CHECK-NEXT:   .byte   8
; CHECK-NEXT:   .short  0
; CHECK-NEXT:   .long   2
; SmallConstant
; CHECK-NEXT:   .byte   4
; CHECK-NEXT:   .byte   8
; CHECK-NEXT:   .short  0
; CHECK-NEXT:   .long   -1

define void @constantargs() {
entry:
  tail call void (i64, i32, ...) @llvm.experimental.stackmap(i64 1, i32 15, i16 65535, i16 -1, i32 65536, i32 2000000000, i32 2147483647, i32 -1, i32 4294967295, i32 4294967296, i64 2147483648, i64 4294967295, i64 4294967296, i64 -1)
  ret void
}

; Map a constant value.
;
; CHECK-LABEL:  .long L{{.*}}-_liveConstant
; CHECK-NEXT:   .short 0
; 1 location
; CHECK-NEXT:   .short 1
; Loc 0: SmallConstant
; CHECK-NEXT:   .byte   4
; CHECK-NEXT:   .byte   8
; CHECK-NEXT:   .short  0
; CHECK-NEXT:   .long   33

define void @liveConstant() {
  tail call void (i64, i32, ...) @llvm.experimental.stackmap(i64 15, i32 5, i32 33)
  ret void
}

; Directly map an alloca's address.
;
; Callsite 16
; CHECK-LABEL:  .long L{{.*}}-_directFrameIdx
; CHECK-NEXT:   .short 0
; 1 location
; CHECK-NEXT:   .short	1
; Loc 0: Direct RBP - ofs
; CHECK-NEXT:   .byte	2
; CHECK-NEXT:   .byte	8
; CHECK-NEXT:   .short	6
; CHECK-NEXT:   .long

define void @directFrameIdx() {
entry:
  %metadata1 = alloca i64, i32 3, align 8
  store i64 11, i64* %metadata1
  store i64 12, i64* %metadata1
  store i64 13, i64* %metadata1
  call void (i64, i32, ...) @llvm.experimental.stackmap(i64 16, i32 0, i64* %metadata1)
  ret void
}

; Test a 64-bit ID.
;
; CHECK:        .quad 4294967295
; CHECK-LABEL:  .long L{{.*}}-_longid
; CHECK:        .quad 4294967296
; CHECK-LABEL:  .long L{{.*}}-_longid
; CHECK:        .quad 9223372036854775807
; CHECK-LABEL:  .long L{{.*}}-_longid
; CHECK:        .quad -1
; CHECK-LABEL:  .long L{{.*}}-_longid
define void @longid() {
entry:
  tail call void (i64, i32, ...) @llvm.experimental.stackmap(i64 4294967295, i32 0)
  tail call void (i64, i32, ...) @llvm.experimental.stackmap(i64 4294967296, i32 0)
  tail call void (i64, i32, ...) @llvm.experimental.stackmap(i64 9223372036854775807, i32 0)
  tail call void (i64, i32, ...) @llvm.experimental.stackmap(i64 -1, i32 0)
  ret void
}

declare void @llvm.experimental.stackmap(i64, i32, ...)

; V2CHECK-LABEL:	.section	__LLVM_STACKMAPS,__llvm_stackmaps
; V2CHECK-NEXT:__LLVM_StackMaps:
; V2CHECK-NEXT:	.byte	2
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	4
; V2CHECK-NEXT:	.long	L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.long	L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.long	L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.long	L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.long	L{{.*}}-__LLVM_StackMaps

; Constant Array
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:L{{.*}}:
; V2CHECK-NEXT:	.quad	2147483648
; V2CHECK-NEXT:	.quad	4294967295
; V2CHECK-NEXT:	.quad	4294967296

; FrameMapRecord Array
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:L{{.*}}:
; V2CHECK-NEXT:	.quad	_constantargs
; V2CHECK-NEXT:	.long	L{{.*}}-_constantargs
; V2CHECK-NEXT:	.long	8
; V2CHECK-NEXT:	.short	1
; V2CHECK-NEXT:	.short	6
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	1
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:	.quad	_liveConstant
; V2CHECK-NEXT:	.long	L{{.*}}-_liveConstant
; V2CHECK-NEXT:	.long	8
; V2CHECK-NEXT:	.short	1
; V2CHECK-NEXT:	.short	6
; V2CHECK-NEXT:	.short	1
; V2CHECK-NEXT:	.short	1
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:	.quad	_directFrameIdx
; V2CHECK-NEXT:	.long	L{{.*}}-_directFrameIdx
; V2CHECK-NEXT:	.long	40
; V2CHECK-NEXT:	.short	1
; V2CHECK-NEXT:	.short	6
; V2CHECK-NEXT:	.short	2
; V2CHECK-NEXT:	.short	1
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:	.quad	_longid
; V2CHECK-NEXT:	.long	Lfunc_end3-_longid
; V2CHECK-NEXT:	.long	8
; V2CHECK-NEXT:	.short	1
; V2CHECK-NEXT:	.short	6
; V2CHECK-NEXT:	.short	3
; V2CHECK-NEXT:	.short	4
; V2CHECK-NEXT:	.align	3

; StackMapRecord Array
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:L{{.*}}:
; V2CHECK-NEXT:	.quad	1
; V2CHECK-NEXT:	.long	L{{.*}}-_constantargs
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	12
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:	.quad	15
; V2CHECK-NEXT:	.long	L{{.*}}-_liveConstant
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.short	12
; V2CHECK-NEXT:	.short	1
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:	.quad	16
; V2CHECK-NEXT:	.long	L{{.*}}-_directFrameIdx
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.short	13
; V2CHECK-NEXT:	.short	1
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:	.quad	4294967295
; V2CHECK-NEXT:	.long	L{{.*}}-_longid
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.short	14
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:	.quad	4294967296
; V2CHECK-NEXT:	.long	L{{.*}}-_longid
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.short	14
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:	.quad	9223372036854775807
; V2CHECK-NEXT:	.long	L{{.*}}-_longid
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.short	14
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:	.quad	-1
; V2CHECK-NEXT:	.long	L{{.*}}-_longid
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.byte	0
; V2CHECK-NEXT:	.short	14
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.align	3

; Location Array
; V2CHECK-NEXT:	.align	2
; V2CHECK-NEXT:L{{.*}}:
; V2CHECK-NEXT:	.byte	4
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	-1
; V2CHECK-NEXT:	.align	2
; V2CHECK-NEXT:	.byte	4
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	-1
; V2CHECK-NEXT:	.align	2
; V2CHECK-NEXT:	.byte	4
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	65536
; V2CHECK-NEXT:	.align	2
; V2CHECK-NEXT:	.byte	4
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	2000000000
; V2CHECK-NEXT:	.align	2
; V2CHECK-NEXT:	.byte	4
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	2147483647
; V2CHECK-NEXT:	.align	2
; V2CHECK-NEXT:	.byte	4
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	-1
; V2CHECK-NEXT:	.align	2
; V2CHECK-NEXT:	.byte	4
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	-1
; V2CHECK-NEXT:	.align	2
; V2CHECK-NEXT:	.byte	4
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	0
; V2CHECK-NEXT:	.align	2
; V2CHECK-NEXT:	.byte	5
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	0
; V2CHECK-NEXT:	.align	2
; V2CHECK-NEXT:	.byte	5
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	1
; V2CHECK-NEXT:	.align	2
; V2CHECK-NEXT:	.byte	5
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	2
; V2CHECK-NEXT:	.align	2
; V2CHECK-NEXT:	.byte	4
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	-1
; V2CHECK-NEXT:	.align	2
; V2CHECK-NEXT:	.byte	4
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	0
; V2CHECK-NEXT:	.long	33
; V2CHECK-NEXT:	.align	2
; V2CHECK-NEXT:	.byte	2
; V2CHECK-NEXT:	.byte	8
; V2CHECK-NEXT:	.short	6
; V2CHECK-NEXT:	.long	-24
; V2CHECK-NEXT:	.align	2
