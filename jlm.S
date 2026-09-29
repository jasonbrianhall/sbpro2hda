/* Glue between C and Jemm's VMM API (INT 20h + service word + device word).
   Register contract: services may change EAX, ECX, EDX; the rest survive. */

#define VMM(n)  int $0x20 ; .word n, 1

#define Get_Cur_VM_Handle       1
#define Allocate_V86_Call_Back  2
#define Simulate_Iret           10
#define Simulate_Far_Call       11
#define Simulate_Far_Jmp        12
#define PageFree                18
#define Begin_Nest_Exec         23
#define Exec_Int                24
#define Resume_Exec             25
#define End_Nest_Exec           26
#define Install_IO_Handler      31
#define Remove_IO_Handler       34
#define PageReserve             39
#define PageDecommit            41
#define PageCommitPhys          42
#define Simulate_Int            9
#define Free_V86_Call_Back      43

        .text

/* Client *jlm_client(void) */
        .globl  _jlm_client
_jlm_client:
        push    %ebx
        VMM(Get_Cur_VM_Handle)
        movl    8(%ebx), %eax           /* CB_Client_Pointer */
        pop     %ebx
        ret

/* void jlm_nest_int(Client *c, int intno) */
        .globl  _jlm_nest_int
_jlm_nest_int:
        push    %ebp
        movl    8(%esp), %ebp
        VMM(Begin_Nest_Exec)
        movl    12(%esp), %eax
        VMM(Exec_Int)
        VMM(End_Nest_Exec)
        pop     %ebp
        ret

/* void jlm_nest_far_call(Client *c, uint32_t segoff) */
        .globl  _jlm_nest_far_call
_jlm_nest_far_call:
        push    %ebp
        movl    8(%esp), %ebp
        VMM(Begin_Nest_Exec)
        movzwl  12(%esp), %edx
        movzwl  14(%esp), %ecx
        VMM(Simulate_Far_Call)
        VMM(Resume_Exec)
        VMM(End_Nest_Exec)
        pop     %ebp
        ret

/* uint32_t jlm_page_reserve(page, npages, flags) - stack args, cdecl */
        .globl  _jlm_page_reserve
_jlm_page_reserve:
        pushl   12(%esp)
        pushl   12(%esp)
        pushl   12(%esp)
        VMM(PageReserve)
        addl    $12, %esp
        ret

/* uint32_t jlm_page_commit_phys(page, npages, physpage, flags) */
        .globl  _jlm_page_commit_phys
_jlm_page_commit_phys:
        pushl   16(%esp)
        pushl   16(%esp)
        pushl   16(%esp)
        pushl   16(%esp)
        VMM(PageCommitPhys)
        addl    $16, %esp
        ret

/* uint32_t jlm_page_decommit(page, npages, flags) */
        .globl  _jlm_page_decommit
_jlm_page_decommit:
        pushl   12(%esp)
        pushl   12(%esp)
        pushl   12(%esp)
        VMM(PageDecommit)
        addl    $12, %esp
        ret

/* uint32_t jlm_page_free(lin, flags) */
        .globl  _jlm_page_free
_jlm_page_free:
        pushl   8(%esp)
        pushl   8(%esp)
        VMM(PageFree)
        addl    $8, %esp
        ret

/* int jlm_install_io(uint16_t port, void (*handler)(void)) - 1 = ok */
        .globl  _jlm_install_io
_jlm_install_io:
        push    %esi
        movzwl  8(%esp), %edx
        movl    12(%esp), %esi
        VMM(Install_IO_Handler)
        movl    $0, %eax
        setnc   %al
        pop     %esi
        ret

/* int jlm_remove_io(uint16_t port) */
        .globl  _jlm_remove_io
_jlm_remove_io:
        movzwl  4(%esp), %edx
        VMM(Remove_IO_Handler)
        movl    $0, %eax
        setnc   %al
        ret

/* uint32_t jlm_alloc_v86_callback(void (*proc)(void), uint32_t refdata) */
        .globl  _jlm_alloc_v86_callback
_jlm_alloc_v86_callback:
        push    %esi
        movl    8(%esp), %esi
        movl    12(%esp), %edx
        VMM(Allocate_V86_Call_Back)
        jnc     1f
        xorl    %eax, %eax
1:      pop     %esi
        ret

/* void jlm_free_v86_callback(uint32_t segoff) */
        .globl  _jlm_free_v86_callback
_jlm_free_v86_callback:
        movl    4(%esp), %eax
        VMM(Free_V86_Call_Back)
        ret

/* I/O trap handler. In: ECX = type, EDX = port, EAX = client EAX,
   EBP = client. Out: EAX = value for IN. */
        .globl  _io_thunk
_io_thunk:
        cld
        push    %ecx
        push    %edx
        push    %eax
        call    _sb_io
        addl    $12, %esp
/* Jemm has already moved the client's IP past the IN/OUT, so a pending
   SB IRQ (F2h, a finished tiny transfer) can be injected right here -
   but only if the game has interrupts enabled. */
        testl   $0x200, 48(%ebp)        /* Client_EFlags.IF */
        jz      3f
        push    %eax
        call    _sb_pending_vector
        testl   %eax, %eax
        jz      2f
        VMM(Simulate_Int)
2:      pop     %eax
3:      ret

/* V86 callback installed as the HDA IRQ vector. EBP = client. */
        .globl  _irq_thunk
_irq_thunk:
        cld
        call    _hda_irq_service
        testl   %eax, %eax
        jz      1f
        VMM(Simulate_Iret)              /* back to the interrupted code... */
        call    _sb_pending_vector
        testl   %eax, %eax
        jz      2f
        VMM(Simulate_Int)               /* ...which first takes the SB IRQ */
2:      ret
1:      movzwl  _irq_chain_vector, %edx         /* not ours: continue at old vector */
        movzwl  _irq_chain_vector+2, %ecx
        VMM(Simulate_Far_Jmp)
        ret

        .data
        .globl  _irq_chain_vector
_irq_chain_vector:
        .long   0
