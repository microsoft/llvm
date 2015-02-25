; RUN: opt -S -lowerbitsets < %s | FileCheck %s
; RUN: opt -S -O3 < %s | FileCheck -check-prefix=CHECK-NODISCARD %s

target datalayout = "e-p:32:32"

; CHECK: [[G:@[^ ]*]] = private constant { i32, [63 x i32], i32, [2 x i32] } { i32 1, [63 x i32] zeroinitializer, i32 3, [2 x i32] [i32 4, i32 5] }
@a = constant i32 1
@b = constant [63 x i32] zeroinitializer
@c = constant i32 3
@d = constant [2 x i32] [i32 4, i32 5]

; Offset 0, 4 byte alignment
; CHECK: @bitset1.bits = private constant [9 x i8] c"\03\00\00\00\00\00\00\00\04"
!0 = !{!"bitset1", i32* @a, i32 0}
; CHECK-NODISCARD-DAG: !{!"bitset1", i32* @a, i32 0}
!1 = !{!"bitset1", [63 x i32]* @b, i32 0}
; CHECK-NODISCARD-DAG: !{!"bitset1", [63 x i32]* @b, i32 0}
!2 = !{!"bitset1", [2 x i32]* @d, i32 4}
; CHECK-NODISCARD-DAG: !{!"bitset1", [2 x i32]* @d, i32 4}

; Offset 4, 4 byte alignment
; CHECK: @bitset2.bits = private constant [8 x i8] c"\01\00\00\00\00\00\00\80"
!3 = !{!"bitset2", [63 x i32]* @b, i32 0}
; CHECK-NODISCARD-DAG: !{!"bitset2", [63 x i32]* @b, i32 0}
!4 = !{!"bitset2", i32* @c, i32 0}
; CHECK-NODISCARD-DAG: !{!"bitset2", i32* @c, i32 0}

; Offset 0, 256 byte alignment
; CHECK: @bitset3.bits = private constant [1 x i8] c"\03"
!5 = !{!"bitset3", i32* @a, i32 0}
; CHECK-NODISCARD-DAG: !{!"bitset3", i32* @a, i32 0}
!6 = !{!"bitset3", i32* @c, i32 0}
; CHECK-NODISCARD-DAG: !{!"bitset3", i32* @c, i32 0}

; Entries whose second operand is null (the result of a global being DCE'd)
; should be ignored.
!7 = !{!"bitset2", null, i32 0}

!llvm.bitsets = !{ !0, !1, !2, !3, !4, !5, !6, !7 }

; CHECK: @a = alias getelementptr inbounds ({ i32, [63 x i32], i32, [2 x i32] }* [[G]], i32 0, i32 0)
; CHECK: @b = alias getelementptr inbounds ({ i32, [63 x i32], i32, [2 x i32] }* [[G]], i32 0, i32 1)
; CHECK: @c = alias getelementptr inbounds ({ i32, [63 x i32], i32, [2 x i32] }* [[G]], i32 0, i32 2)
; CHECK: @d = alias getelementptr inbounds ({ i32, [63 x i32], i32, [2 x i32] }* [[G]], i32 0, i32 3)

declare i1 @llvm.bitset.test(i8* %ptr, metadata %bitset) nounwind readnone

; CHECK: @foo(i32* [[A0:%[^ ]*]])
define i1 @foo(i32* %p) {
  ; CHECK-NOT: llvm.bitset.test

  ; CHECK: [[R0:%[^ ]*]] = bitcast i32* [[A0]] to i8*
  %pi8 = bitcast i32* %p to i8*
  ; CHECK: [[R1:%[^ ]*]] = ptrtoint i8* [[R0]] to i32
  ; CHECK: [[R2:%[^ ]*]] = sub i32 [[R1]], ptrtoint ({ i32, [63 x i32], i32, [2 x i32] }* [[G]] to i32)
  ; CHECK: [[R3:%[^ ]*]] = lshr i32 [[R2]], 2
  ; CHECK: [[R4:%[^ ]*]] = shl i32 [[R2]], 30
  ; CHECK: [[R5:%[^ ]*]] = or i32 [[R3]], [[R4]]
  ; CHECK: [[R6:%[^ ]*]] = icmp ult i32 [[R5]], 67
  ; CHECK: br i1 [[R6]]

  ; CHECK: [[R8:%[^ ]*]] = lshr i32 [[R5]], 5
  ; CHECK: [[R9:%[^ ]*]] = getelementptr i32* bitcast ([9 x i8]* @bitset1.bits to i32*), i32 [[R8]]
  ; CHECK: [[R10:%[^ ]*]] = load i32* [[R9]]
  ; CHECK: [[R11:%[^ ]*]] = and i32 [[R5]], 31
  ; CHECK: [[R12:%[^ ]*]] = shl i32 1, [[R11]]
  ; CHECK: [[R13:%[^ ]*]] = and i32 [[R10]], [[R12]]
  ; CHECK: [[R14:%[^ ]*]] = icmp ne i32 [[R13]], 0

  ; CHECK: [[R16:%[^ ]*]] = phi i1 [ false, {{%[^ ]*}} ], [ [[R14]], {{%[^ ]*}} ]
  %x = call i1 @llvm.bitset.test(i8* %pi8, metadata !"bitset1")

  ; CHECK-NOT: llvm.bitset.test
  %y = call i1 @llvm.bitset.test(i8* %pi8, metadata !"bitset1")

  ; CHECK: ret i1 [[R16]]
  ret i1 %x
}

; CHECK: @bar(i32* [[B0:%[^ ]*]])
define i1 @bar(i32* %p) {
  ; CHECK: [[S0:%[^ ]*]] = bitcast i32* [[B0]] to i8*
  %pi8 = bitcast i32* %p to i8*
  ; CHECK: [[S1:%[^ ]*]] = ptrtoint i8* [[S0]] to i32
  ; CHECK: [[S2:%[^ ]*]] = sub i32 [[S1]], add (i32 ptrtoint ({ i32, [63 x i32], i32, [2 x i32] }* [[G]] to i32), i32 4)
  ; CHECK: [[S3:%[^ ]*]] = lshr i32 [[S2]], 2
  ; CHECK: [[S4:%[^ ]*]] = shl i32 [[S2]], 30
  ; CHECK: [[S5:%[^ ]*]] = or i32 [[S3]], [[S4]]
  ; CHECK: [[S6:%[^ ]*]] = icmp ult i32 [[S5]], 64
  ; CHECK: br i1 [[S6]]

  ; CHECK: [[S8:%[^ ]*]] = zext i32 [[S5]] to i64
  ; CHECK: [[S9:%[^ ]*]] = and i64 [[S8]], 63
  ; CHECK: [[S10:%[^ ]*]] = shl i64 1, [[S9]]
  ; CHECK: [[S11:%[^ ]*]] = and i64 -9223372036854775807, [[S10]]
  ; CHECK: [[S12:%[^ ]*]] = icmp ne i64 [[S11]], 0

  ; CHECK: [[S16:%[^ ]*]] = phi i1 [ false, {{%[^ ]*}} ], [ [[S12]], {{%[^ ]*}} ]
  %x = call i1 @llvm.bitset.test(i8* %pi8, metadata !"bitset2")
  ; CHECK: ret i1 [[S16]]
  ret i1 %x
}

; CHECK: @baz(i32* [[C0:%[^ ]*]])
define i1 @baz(i32* %p) {
  ; CHECK: [[T0:%[^ ]*]] = bitcast i32* [[C0]] to i8*
  %pi8 = bitcast i32* %p to i8*
  ; CHECK: [[T1:%[^ ]*]] = ptrtoint i8* [[T0]] to i32
  ; CHECK: [[T2:%[^ ]*]] = sub i32 [[T1]], ptrtoint ({ i32, [63 x i32], i32, [2 x i32] }* [[G]] to i32)
  ; CHECK: [[T3:%[^ ]*]] = lshr i32 [[T2]], 8
  ; CHECK: [[T4:%[^ ]*]] = shl i32 [[T2]], 24
  ; CHECK: [[T5:%[^ ]*]] = or i32 [[T3]], [[T4]]
  ; CHECK: [[T6:%[^ ]*]] = icmp ult i32 [[T5]], 2
  ; CHECK: br i1 [[T6]]

  ; CHECK: [[T8:%[^ ]*]] = and i32 [[T5]], 31
  ; CHECK: [[T9:%[^ ]*]] = shl i32 1, [[T8]]
  ; CHECK: [[T10:%[^ ]*]] = and i32 3, [[T9]]
  ; CHECK: [[T11:%[^ ]*]] = icmp ne i32 [[T10]], 0

  ; CHECK: [[T16:%[^ ]*]] = phi i1 [ false, {{%[^ ]*}} ], [ [[T11]], {{%[^ ]*}} ]
  %x = call i1 @llvm.bitset.test(i8* %pi8, metadata !"bitset3")
  ; CHECK: ret i1 [[T16]]
  ret i1 %x
}

; CHECK-NOT: !llvm.bitsets
