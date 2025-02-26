; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc < %s -mtriple=x86_64-apple-darwin -mcpu=skx  -mattr=+avx512vbmi --show-mc-encoding| FileCheck %s
declare <64 x i8> @llvm.x86.avx512.mask.permvar.qi.512(<64 x i8>, <64 x i8>, <64 x i8>, i64)

define <64 x i8>@test_int_x86_avx512_mask_permvar_qi_512(<64 x i8> %x0, <64 x i8> %x1, <64 x i8> %x2, i64 %x3) {
; CHECK-LABEL: test_int_x86_avx512_mask_permvar_qi_512:
; CHECK:       ## BB#0:
; CHECK-NEXT:    kmovq %rdi, %k1 
; CHECK-NEXT:    vpermb %zmm1, %zmm0, %zmm2 {%k1} 
; CHECK-NEXT:    vpermb %zmm1, %zmm0, %zmm3 {%k1} {z} 
; CHECK-NEXT:    vpermb %zmm1, %zmm0, %zmm0 
; CHECK-NEXT:    vpaddb %zmm3, %zmm2, %zmm1 
; CHECK-NEXT:    vpaddb %zmm0, %zmm1, %zmm0 
; CHECK-NEXT:    retq 
 %res = call <64 x i8> @llvm.x86.avx512.mask.permvar.qi.512(<64 x i8> %x0, <64 x i8> %x1, <64 x i8> %x2, i64 %x3)
 %res1 = call <64 x i8> @llvm.x86.avx512.mask.permvar.qi.512(<64 x i8> %x0, <64 x i8> %x1, <64 x i8> zeroinitializer, i64 %x3)
 %res2 = call <64 x i8> @llvm.x86.avx512.mask.permvar.qi.512(<64 x i8> %x0, <64 x i8> %x1, <64 x i8> %x2, i64 -1)
 %res3 = add <64 x i8> %res, %res1
 %res4 = add <64 x i8> %res3, %res2
 ret <64 x i8> %res4
}
