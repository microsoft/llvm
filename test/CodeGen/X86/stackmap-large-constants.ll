; RUN: llc < %s -mtriple=x86_64-apple-darwin | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-apple-darwin -stackmap-version=2 | FileCheck %s --check-prefix=V2CHECK

; CHECK-LABEL:	.section	__LLVM_STACKMAPS,__llvm_stackmaps
; CHECK-NEXT: __LLVM_StackMaps:
; version
; CHECK-NEXT: 	.byte	1
; reserved
; CHECK-NEXT: 	.byte	0
; reserved
; CHECK-NEXT: 	.short	0
; # functions
; CHECK-NEXT: 	.long	2
; # constants
; CHECK-NEXT: 	.long	2
; # records
; CHECK-NEXT: 	.long	2
; function address & stack size
; CHECK-NEXT: 	.quad	_foo
; CHECK-NEXT: 	.quad	8
; function address & stack size
; CHECK-NEXT: 	.quad	_bar
; CHECK-NEXT: 	.quad	8

; Constants Array:
; CHECK-NEXT: 	.quad	9223372036854775807
; CHECK-NEXT: 	.quad	-9223372036854775808

; Patchpoint ID
; CHECK-NEXT: 	.quad	0
; Instruction offset
; CHECK-NEXT: 	.long	L{{.*}}-_foo
; reserved
; CHECK-NEXT: 	.short	0
; # locations
; CHECK-NEXT: 	.short	1
; ConstantIndex
; CHECK-NEXT: 	.byte	5
; reserved
; CHECK-NEXT: 	.byte	8
; Dwarf RegNum
; CHECK-NEXT: 	.short	0
; Offset
; CHECK-NEXT: 	.long	0
; padding
; CHECK-NEXT: 	.short	0
; NumLiveOuts
; CHECK-NEXT: 	.short	0

; CHECK-NEXT: 	.align	3

declare void @llvm.experimental.stackmap(i64, i32, ...)

define void @foo() {
  tail call void (i64, i32, ...) @llvm.experimental.stackmap(i64 0, i32 0, i64 9223372036854775807)
  ret void
}

; Patchpoint ID
; CHECK-NEXT: 	.quad	0
; Instruction Offset
; CHECK-NEXT: 	.long	L{{.*}}-_bar
; reserved
; CHECK-NEXT: 	.short	0
; # locations
; CHECK-NEXT: 	.short	1
; ConstantIndex
; CHECK-NEXT: 	.byte	5
; reserved
; CHECK-NEXT: 	.byte	8
; Dwarf RegNum
; CHECK-NEXT: 	.short	0
; Offset
; CHECK-NEXT: 	.long	1
; padding
; CHECK-NEXT: 	.short	0
; NumLiveOuts
; CHECK-NEXT: 	.short	0


define void @bar() {
  tail call void (i64, i32, ...) @llvm.experimental.stackmap(i64 0, i32 0, i64 -9223372036854775808)
  ret void
}

; V2CHECK-LABEL:	.section	__LLVM_STACKMAPS,__llvm_stackmaps
; V2CHECK-NEXT: __LLVM_StackMaps:
; version
; V2CHECK-NEXT: .byte	2
; reserved
; V2CHECK-NEXT:	.byte	0
; reserved
; V2CHECK-NEXT:	.short	0
; # functions
; V2CHECK-NEXT:	.long	2
; V2CHECK-NEXT:	.long	L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.long	L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.long	L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.long	L{{.*}}-__LLVM_StackMaps
; V2CHECK-NEXT:	.long	L{{.*}}-__LLVM_StackMaps

; Constants Array:
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:L{{.*}}:
; V2CHECK-NEXT:	.quad	9223372036854775807
; V2CHECK-NEXT:	.quad	-9223372036854775808

; FrameRecord Array
; padding
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:L{{.*}}:
; function address
; V2CHECK-NEXT:	.quad	_foo
; function size
; V2CHECK-NEXT:	.long	L{{.*}}-_foo
; stack size
; V2CHECK-NEXT:	.long	8
; flags
; V2CHECK-NEXT:	.short	1
; frame base register
; V2CHECK-NEXT:	.short	6
; stackmap record index
; V2CHECK-NEXT:	.short	0
; stackmap record size
; V2CHECK-NEXT:	.short	1
; padding
; V2CHECK-NEXT:	.align	3

; function address
; V2CHECK-NEXT:	.quad	_bar
; function size
; V2CHECK-NEXT:	.long	L{{.*}}-_bar
; stack size
; V2CHECK-NEXT:	.long	8
; flags
; V2CHECK-NEXT:	.short	1
; frame base register
; V2CHECK-NEXT:	.short	6
; stackmap record index
; V2CHECK-NEXT:	.short	1
; stackmap record size
; V2CHECK-NEXT:	.short	1
; padding
; V2CHECK-NEXT:	.align	3

; StackMapRecord Array
; padding
; V2CHECK-NEXT:	.align	3
; V2CHECK-NEXT:L{{.*}}:
; ID
; V2CHECK-NEXT:	.quad	0
; instruction offset
; V2CHECK-NEXT:	.long	L{{.*}}-_foo
; call size
; V2CHECK-NEXT:	.byte	0
; flags
; V2CHECK-NEXT:	.byte	0
; location index
; V2CHECK-NEXT:	.short	0
; # locations
; V2CHECK-NEXT:	.short	1
; liveout index
; V2CHECK-NEXT:	.short	0
; # liveouts
; V2CHECK-NEXT:	.short	0
; padding
; V2CHECK-NEXT:	.align	3

; ID
; V2CHECK-NEXT:	.quad	0
; instruction offset
; V2CHECK-NEXT:	.long	L{{.*}}-_bar
; call size
; V2CHECK-NEXT:	.byte	0
; flags
; V2CHECK-NEXT:	.byte	0
; location index
; V2CHECK-NEXT:	.short	1
; # locations
; V2CHECK-NEXT:	.short	1
; liveout index
; V2CHECK-NEXT:	.short	0
; # liveouts
; V2CHECK-NEXT:	.short	0
; padding
; V2CHECK-NEXT:	.align	3

; Location Array
; padding
; V2CHECK-NEXT:	.align	2
; V2CHECK-NEXT:L{{.*}}:
; type
; V2CHECK-NEXT:	.byte	5
; size
; V2CHECK-NEXT:	.byte	8
; Dwarf RegNum
; V2CHECK-NEXT:	.short	0
; offset
; V2CHECK-NEXT:	.long	0
; padding
; V2CHECK-NEXT:	.align	2

; type
; V2CHECK-NEXT:	.byte	5
; size
; V2CHECK-NEXT:	.byte	8
; Dwarf RegNum
; V2CHECK-NEXT:	.short	0
; offset
; V2CHECK-NEXT:	.long	1
; padding
; V2CHECK-NEXT:	.align	2
