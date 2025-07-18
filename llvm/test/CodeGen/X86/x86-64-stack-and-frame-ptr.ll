; RUN: llc -verify-machineinstrs -mtriple=x86_64-pc-linux < %s | FileCheck %s
; RUN: llc -verify-machineinstrs -mtriple=x86_64-pc-linux-gnux32 < %s | FileCheck -check-prefix=X32ABI %s

; x32 uses %esp, %ebp as stack and frame pointers

; CHECK-LABEL: foo
; CHECK: pushq %rbp
; CHECK: movq %rsp, %rbp
; CHECK: movq %rdi, -8(%rbp)
; CHECK: popq %rbp
; X32ABI-LABEL: foo
; X32ABI: pushq %rbp
; X32ABI: movl %esp, %ebp
; X32ABI: movl %edi, -4(%ebp)
; X32ABI: popq %rbp

define void @foo(ptr %a) #0 {
entry:
  %a.addr = alloca ptr, align 4
  %b = alloca ptr, align 4
  store ptr %a, ptr %a.addr, align 4
  ret void
}

attributes #0 = { nounwind uwtable "frame-pointer"="all"}
