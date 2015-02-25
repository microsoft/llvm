; RUN: llvm-as < %s | llvm-dis | llvm-as | llvm-dis | FileCheck %s
; RUN: verify-uselistorder %s

; CHECK: !named = !{!0, !1, !2, !3, !4, !4, !5, !6, !7, !7}
!named = !{!0, !1, !2, !3, !4, !5, !6, !7, !8, !9}

!0 = distinct !{}
!1 = distinct !{}
!2 = !MDFile(filename: "path/to/file", directory: "/path/to/dir")

; CHECK: !3 = !MDLexicalBlock(scope: !0, file: !2, line: 7, column: 35)
!3 = !MDLexicalBlock(scope: !0, file: !2, line: 7, column: 35)

; CHECK: !4 = !MDLexicalBlock(scope: !0)
!4 = !MDLexicalBlock(scope: !0)
!5 = !MDLexicalBlock(scope: !0, file: null, line: 0, column: 0)

; CHECK: !5 = !MDLexicalBlockFile(scope: !3, file: !2, discriminator: 0)
; CHECK: !6 = !MDLexicalBlockFile(scope: !3, file: !2, discriminator: 1)
!6 = !MDLexicalBlockFile(scope: !3, file: !2, discriminator: 0)
!7 = !MDLexicalBlockFile(scope: !3, file: !2, discriminator: 1)

; CHECK: !7 = !MDLexicalBlockFile(scope: !3, discriminator: 7)
!8 = !MDLexicalBlockFile(scope: !3, discriminator: 7)
!9 = !MDLexicalBlockFile(scope: !3, file: null, discriminator: 7)
