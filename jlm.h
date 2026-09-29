#ifndef JLM_H
#define JLM_H
#include <stdint.h>

/* The subset of Jemm's VMM API (a Win9x VMM look-alike) that SBPRO uses.
   Implemented in jlm.S. */

typedef struct {
    uint32_t EDI, ESI, EBP, res0, EBX, EDX, ECX, EAX;
    uint32_t Int, Error, EIP, CS, EFlags, ESP, SS, ES, DS, FS, GS;
} Client;

typedef struct {                /* 3rd DllMain argument */
    uint16_t wLdrCS;
    uint16_t wFlags;
    uint32_t lpCmdLine;         /* linear, ends at CR or NUL */
    uint32_t lpRequest;
} JLCOMM;

#define JLF_UNLOAD  1
#define JLF_DRIVER  2

typedef struct __attribute__((packed)) {
    uint32_t Next;
    uint16_t Version;
    uint16_t Req_Device_Number;
    uint8_t  Dev_Major_Version;
    uint8_t  Dev_Minor_Version;
    uint16_t Flags;
    char     Name[8];
    uint32_t Init_Order;
    uint32_t Control_Proc;
    uint32_t V86_API_Proc;
    uint32_t PM_API_Proc;
    uint32_t V86_API_CSIP;
    uint32_t PM_API_CSIP;
    uint32_t Reference_Data;
    uint32_t Service_Table_Ptr;
    uint32_t Service_Table_Size;
    uint32_t Win32_Service_Table;
    uint32_t Prev;
    uint32_t Size;
    uint32_t Reserved1, Reserved2, Reserved3;
} DDB;

#define PR_SYSTEM    0x80080000u
#define PC_WRITEABLE 0x00020000u
#define PC_INCR      0x40000000u

/* I/O trap type bits (ECX in the handler) */
#define IO_OUTPUT 0x04
#define IO_WORD   0x08
#define IO_DWORD  0x10

uint32_t jlm_version(void);             /* minor << 16 | major */
Client  *jlm_client(void);
void     jlm_nest_int(Client *c, int intno);
void     jlm_nest_far_call(Client *c, uint32_t segoff);
uint32_t jlm_page_reserve(uint32_t page, uint32_t npages, uint32_t flags);
uint32_t jlm_page_commit_phys(uint32_t page, uint32_t npages, uint32_t physpage, uint32_t flags);
uint32_t jlm_page_decommit(uint32_t page, uint32_t npages, uint32_t flags);
uint32_t jlm_page_free(uint32_t lin, uint32_t flags);
int      jlm_install_io(uint16_t port, void (*handler)(void));
int      jlm_remove_io(uint16_t port);
uint32_t jlm_alloc_v86_callback(void (*proc)(void), uint32_t refdata);   /* seg:off, 0 = fail */
void     jlm_free_v86_callback(uint32_t segoff);
void     jlm_simulate_int(Client *c, int vec);

/* Assembly entry points that call into C */
void io_thunk(void);            /* -> sb_io() */
void irq_thunk(void);           /* -> hda_irq_service(), then IRET or chain */
void sbret_thunk(void);         /* the game's SB handler has returned */
extern uint32_t irq_chain_vector;

uint32_t sb_io(uint32_t data, uint32_t port, uint32_t type);
int      hda_irq_service(void);

/* Console output during load (DOS via nested execution). */
void jprintf(const char *fmt, ...);

/* Debug log to COM1, 115200 8N1. Enabled by /D. */
extern int dbg_on;
void dbg_init(void);
void dbg(const char *fmt, ...);

#endif
