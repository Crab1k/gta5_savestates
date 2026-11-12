; uint64_t RunOnStack(void* top, uint64_t (*fn)(void* a, void* b), void* a, void* b)
; Calls fn(a, b) on the stack that ends at `top` (16-byte aligned) and returns its result on the original stack.
; The way back is a machine frame at the top of the new stack: [top-28h] our return address, [top-10h] the caller's
; stack pointer after the return. Fixed places: a savestate of another game session puts its own values there, and
; the load replaces the stack pointer with this thread's (threads::WayBack); the return address is the same code.
; The unwinder walks from the new stack back to the caller through the same machine frame (exceptions, crash logs),
; whatever the registers hold.
.code
RunOnStack PROC FRAME
    mov rax, [rsp]
    lea r10, [rsp+8]
    mov [rcx-28h], rax                  ; RIP
    mov qword ptr [rcx-20h], 33h        ; CS
    mov qword ptr [rcx-18h], 202h       ; EFLAGS
    mov [rcx-10h], r10                  ; RSP
    mov qword ptr [rcx-8h], 2Bh         ; SS
    lea rsp, [rcx-28h]
    .pushframe
    sub rsp, 28h
    .allocstack 28h
    .endprolog
    mov rax, rdx
    mov rcx, r8
    mov rdx, r9
    call rax
    mov r10, [rsp+28h]
    mov rsp, [rsp+40h]
    jmp r10
RunOnStack ENDP
END
