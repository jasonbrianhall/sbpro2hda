/* HDPMI32i port-trap entry points for SBPM.
 *
 * HDPMI calls these like a 32-bit DPMI exception handler, on its locked
 * stack, with the game's general registers live:
 *   [esp+0]  return EIP to host     [esp+12] faulting EIP
 *   [esp+4]  return CS              [esp+16] CS, [esp+20] EFLAGS
 *   [esp+8]  error code (see sbpm.h; bits 0-2 = instruction size)
 * We switch to our own stack (C code expects SS == DS), call pm_trap(),
 * put the IN result in the game's EAX and step EIP past the instruction.
 * The stack is split in levels because a trap can nest: the SB IRQ may be
 * delivered to the game's handler while we're forwarding a port access.
 */
#define LEVELS 8
#define LEVEL_SIZE 4096

        .data
        .globl  _pm_ds
_pm_ds: .word   0
        .globl  _hold_sel
_hold_sel: .word 0
        .align  4
depth:  .long   0

        .bss
        .align  16
        .lcomm  stacks, LEVELS * LEVEL_SIZE

        .text
        .globl  _trap_in, _trap_out
_trap_in:
        pushl   $0
        jmp     common
_trap_out:
        pushl   $1
common:
        pushal                          /* 32 bytes */
        pushl   %ds
        pushl   %es
        /* layout: 0 es, 4 ds, 8 pushal (edi..eax), 40 dir, 44 host frame */
        movl    40(%esp), %ebp          /* 0 = IN, 1 = OUT */
        movl    52(%esp), %ebx          /* error code */
        movl    28(%esp), %ecx          /* game's EDX */
        movl    36(%esp), %edx          /* game's EAX */
        movl    %esp, %esi
        movl    %ss, %edi

        movw    %cs:_pm_ds, %ax
        movw    %ax, %ds
        movw    %ax, %es
        movl    depth, %eax
        cmpl    $LEVELS - 1, %eax
        jbe     1f
        movl    $LEVELS - 1, %eax
1:      incl    depth
        shll    $12, %eax
        leal    stacks + LEVEL_SIZE(%eax), %eax
        movw    %cs:_pm_ds, %ss
        movl    %eax, %esp
        pushl   %edi                    /* old ss */
        pushl   %esi                    /* old esp */

        pushl   %edx
        pushl   %ecx
        pushl   %ebx
        pushl   %ebp
        cld
        call    _pm_trap
        addl    $16, %esp

        popl    %esi
        popl    %edi
        decl    depth
        movw    %di, %ss
        movl    %esi, %esp

        movl    %eax, 36(%esp)          /* game's EAX (IN result) */
        movl    52(%esp), %ebx
        andl    $7, %ebx
        addl    %ebx, 56(%esp)          /* skip the IN/OUT instruction */

        popl    %es
        popl    %ds
        popal
        addl    $4, %esp
        lret

/* CLI trap (HDPMI API function 9).
 *
 * Under HDPMI32i (IOPL 0) a ring-3 CLI traps and really clears IF, but POPF
 * cannot set IF again at IOPL 0. DOS code saves and restores the interrupt
 * state with "pushf / cli ... popf" (Watcom: "pushfd / pop eax / cli ...
 * push eax / popfd"), so after the first such critical section interrupts
 * would stay off for good: DMX (DOOM, Heretic, ...) then freezes in its first
 * timer wait. HDPMI has already cleared IF when it calls us, with an IRET
 * frame on the game's stack that returns just past the CLI.
 *
 * If the CLI follows one of those save sequences, we look at the saved flags:
 * interrupts were on, so they're switched back on (the POPF that would have
 * done it can't). Every other CLI - an interrupt handler's, or one paired with
 * STI - is left as it is. Must preserve all registers. */
        .globl  _trap_cli
_trap_cli:
        pushal                          /* 0 edi .. 28 eax */
        pushl   %ds
        /* 36: EIP (just past the CLI), 40: CS, 44: EFLAGS, 48: game's stack */
        movl    36(%esp), %ebx
        movl    40(%esp), %eax
        verr    %ax
        jnz     9f
        movw    %ax, %ds
        cmpl    $3, %ebx
        jb      9f
        cmpb    $0x9C, -2(%ebx)         /* pushf ; cli */
        jne     1f
        movl    %ss:48(%esp), %eax
        jmp     8f
1:      cmpb    $0x9C, -3(%ebx)         /* pushf ; pop r ; cli */
        jne     9f
        movzbl  -2(%ebx), %eax
        subl    $0x58, %eax
        cmpl    $7, %eax
        ja      9f
        cmpl    $4, %eax                /* pop esp: not a flags copy */
        je      9f
        negl    %eax
        movl    %ss:32(%esp,%eax,4), %eax  /* saved register from pushal */
8:      testl   $0x200, %eax
        jz      9f
        movw    %cs:_hold_sel, %ax      /* ask SBPRO to hold the SB IRQ back */
        movw    %ax, %ds
        movb    $1, 0x10
        sti                             /* emulated by HDPMI */
9:      popl    %ds
        popal
        iret
