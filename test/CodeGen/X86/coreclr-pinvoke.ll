; RUN: llc -mtriple=x86_64-pc-win32-coreclr -o - < %s | FileCheck %s

; IR for pinvoke stub generated for
;   [DllImport("foo.dll")]
;   static extern void foo();

@"CORINFO_HELP_THROWNULLREF::JitHelper" = external global i64
@"CORINFO_HELP_INIT_PINVOKE_FRAME::JitHelper" = external global i64
@CaptureThreadGlobal = external constant i64
@"CORINFO_HELP_STOP_FOR_GC::JitHelper" = external constant i64

; CHECK-LABEL: .seh_proc DomainBoundILStubClass.IL_STUB_PInvoke
define cc18 void @DomainBoundILStubClass.IL_STUB_PInvoke(i64 %param0) #0 gc "coreclr" {
entry:
  %"$SecretParam" = alloca i64
  %loc0 = alloca i32
  %InlinedCallFrame = alloca [64 x i8]
  %ThreadPointer = alloca [0 x i8]*
  %0 = getelementptr inbounds [64 x i8], [64 x i8]* %InlinedCallFrame, i32 0, i32 8
; CHECK: callq "CORINFO_HELP_INIT_PINVOKE_FRAME::JitHelper"
  %1 = call [0 x i8]* bitcast (i64* @"CORINFO_HELP_INIT_PINVOKE_FRAME::JitHelper" to [0 x i8]* (i8*, i64)*)(i8* %0, i64 %param0)
  store [0 x i8]* %1, [0 x i8]** %ThreadPointer
  %2 = getelementptr inbounds [64 x i8], [64 x i8]* %InlinedCallFrame, i32 0, i32 40
  %3 = bitcast i8* %2 to i64*
  %4 = call i64 @llvm.read_register.i64(metadata !0)
  store i64 %4, i64* %3
  %5 = getelementptr inbounds [64 x i8], [64 x i8]* %InlinedCallFrame, i32 0, i32 56
  %6 = bitcast i8* %5 to i64*
  %7 = call i64 @llvm.read_register.i64(metadata !1)
  store i64 %7, i64* %6
  store i64 %param0, i64* %"$SecretParam"
  br label %8

; <label>:8                                       ; preds = %entry
  store i32 0, i32* %loc0
  %9 = add i64 %param0, 72
  %10 = inttoptr i64 %9 to i64*
  %NullCheck = icmp eq i64* %10, null
  br i1 %NullCheck, label %ThrowNullRef, label %11

; <label>:11                                      ; preds = %8
  %12 = load i64, i64* %10, align 8
  %13 = inttoptr i64 %12 to i64*
  %NullCheck1 = icmp eq i64* %13, null
  br i1 %NullCheck1, label %ThrowNullRef2, label %14

; <label>:14                                      ; preds = %11
  %15 = load i64, i64* %13, align 8
  %16 = inttoptr i64 %15 to void ()*
  %17 = getelementptr inbounds [64 x i8], [64 x i8]* %InlinedCallFrame, i32 0, i32 24
  %18 = bitcast i8* %17 to i64*
  store i64 %param0, i64* %18
  %19 = getelementptr inbounds [64 x i8], [64 x i8]* %InlinedCallFrame, i32 0, i32 8
  %20 = load [0 x i8]*, [0 x i8]** %ThreadPointer
  %21 = getelementptr inbounds [0 x i8], [0 x i8]* %20, i32 0, i32 16
  %22 = bitcast i8* %21 to i8**
  store i8* %19, i8** %22
  %23 = getelementptr inbounds [64 x i8], [64 x i8]* %InlinedCallFrame, i32 0, i32 48
  %24 = bitcast i8* %23 to i8**
  %25 = getelementptr inbounds [0 x i8], [0 x i8]* %20, i32 0, i32 12
  %26 = load i64, i64* @"CORINFO_HELP_STOP_FOR_GC::JitHelper"
; Check for the helper and address after call being stored as arguments to the call.
; CHECK: movq "CORINFO_HELP_STOP_FOR_GC::JitHelper"(%rip), [[Reg1:%[^ ]+]]
; CHECK-NEXT: movq [[Reg1]]
; CHECK-NEXT: leaq [[LabelAfterCall:\.[^ (]+]](%rip), [[Reg2:%[^ ]+]]
; CHECK-NEXT: movq [[Reg2]]
; CHECK: callq {{.+$}}
; CHECK-NOT: {{^ }}
; CHECK: [[LabelAfterCall]]:
  %27 = call i32 (i64, i32, void ()*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_isVoidf(i64 0, i32 0, void ()* %16, i32 0, i32 1, i32 4, i8** %24, i8* %25, i32* bitcast (i64* @CaptureThreadGlobal to i32*), i64 %26, i32 0)
  store i8* null, i8** %24
  %28 = getelementptr inbounds [64 x i8], [64 x i8]* %InlinedCallFrame, i32 0, i32 16
  %29 = bitcast i8* %28 to i8**
  %30 = load i8*, i8** %29
  store i8* %30, i8** %22
  ret void

ThrowNullRef:                                     ; preds = %8
  call void bitcast (i64* @"CORINFO_HELP_THROWNULLREF::JitHelper" to void ()*)() #2
  unreachable

ThrowNullRef2:                                    ; preds = %11
  call void bitcast (i64* @"CORINFO_HELP_THROWNULLREF::JitHelper" to void ()*)() #2
  unreachable
}

; Function Attrs: nounwind readonly
declare i64 @llvm.read_register.i64(metadata) #1

declare i32 @llvm.experimental.gc.statepoint.p0f_isVoidf(i64, i32, void ()*, i32, i32, ...)

attributes #0 = { "no-frame-pointer-elim"="true" }
attributes #1 = { nounwind readonly }
attributes #2 = { noreturn }

!0 = !{!"rsp"}
!1 = !{!"rbp"}
