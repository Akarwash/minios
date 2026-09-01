; ============================================================================
; The signal trampoline: how a handler gets back out.
; ============================================================================
; When the kernel delivers a signal it forges a call frame on the program's own
; stack and points RIP at the handler. A handler is an ordinary C function, so
; when it finishes it executes `ret`, which pops a return address and jumps to
; it. There is no real caller, so the kernel writes THIS address there.
;
; So the handler "returns" into these two instructions, which ask the kernel to
; put the interrupted context back. It never returns from that; SYS_SIGRETURN
; restores the whole saved frame and the program resumes exactly where the signal
; interrupted it, with no idea any of this happened.
;
; WHY THIS LIVES IN THE PROGRAM AND NOT IN THE KERNEL. The kernel cannot pick an
; address for it. Every program is a separately linked ELF at a fixed 0x400000 and
; there is no kernel-owned page mapped into a program's address space, so there is
; nowhere kernel-side that ring-3 code could actually execute. Instead the program
; supplies it: userlib.h passes this symbol's address on the first sys_signal, and
; the kernel stores it per task. Linux solves the same problem the same way — its
; trampoline is in the vDSO, a page the kernel maps into every process — and the
; reason is identical.
;
; Assembled into every user program (see the Makefile's USER_TRAMPOLINE_OBJ).
; Keeping it in a .asm file rather than inline asm in userlib.h keeps it out of
; the compiler's hands entirely: this must be exactly these instructions at a
; stable address, and nothing here should be reachable by an optimiser.

; MANUAL COUPLING, NO COMPILER CHECK. KEEP IN SYNC WITH include/syscalls.h AND
; include/vectors.h. NASM cannot include a C header, so both numbers are
; duplicated here as `equ`s — the same arrangement, for the same reason, as the
; one at the top of kernel/isr_stubs.asm. If either number moves, move it here
; too. Nothing will warn you: the symptom of a stale SYS_SIGRETURN is a handler
; that returns into some other syscall entirely.
SYS_SIGRETURN   equ 16
SYSCALL_VECTOR  equ 0x50

section .text

global sigreturn_trampoline
sigreturn_trampoline:
    ; NO PROLOGUE, NO STACK ADJUSTMENT, AND NOTHING SAVED. There is nothing to
    ; save: this is not a function that was called and it never returns to
    ; anybody. Its whole job is to raise the syscall.
    mov rax, SYS_SIGRETURN
    int SYSCALL_VECTOR

    ; UNREACHABLE. SYS_SIGRETURN overwrites the entire saved register frame with
    ; the one captured at delivery, so the iretq at the end of the kernel's
    ; syscall stub returns to the interrupted instruction, not to here. If control
    ; ever did arrive at this halt, sigreturn failed to redirect the frame and
    ; stopping is far better than running on through whatever follows.
.unreachable:
    hlt
    jmp .unreachable
