; KBTEST.COM - does every arrow key press and release reach the game's
; keyboard handler while SB sound plays? Counts E0 4B/CB (left) and
; E0 4D/CD (right) make/break codes with a game-style INT 9 handler while
; 8-bit auto-init DMA runs at A220 I5 D1. ESC ends and prints the counts
; (hex) to the screen and COM2. The SB handler enables interrupts before
; its EOI, as some games do, so keyboard IRQs can land inside it.
        org 100h

BASE    equ 220h
BUFLEN  equ 2048

start:
        cmp byte [82h], 'N'     ; KBTEST N: SB handler sends no EOI
        jne .e
        mov byte [noeoi], 1
.e:
        ; vectors
        mov ax, 3509h
        int 21h
        mov [old9], bx
        mov [old9+2], es
        mov ax, 350Dh
        int 21h
        mov [oldd], bx
        mov [oldd+2], es
        mov ax, 2509h
        mov dx, kb_isr
        int 21h
        mov ax, 250Dh
        mov dx, sb_isr
        int 21h

        ; buffer: a sawtooth, audible but quiet
        mov di, buf
        mov cx, BUFLEN
        xor al, al
.f:     mov [di], al
        add al, 3
        inc di
        loop .f

        ; DSP reset
        mov dx, BASE+6
        mov al, 1
        out dx, al
        mov cx, 100
.d:     in al, 80h
        loop .d
        xor al, al
        out dx, al
        mov cx, 1000
.r:     mov dx, BASE+0Eh
        in al, dx
        test al, 80h
        jz .rn
        mov dx, BASE+0Ah
        in al, dx
        cmp al, 0AAh
        je .ok
.rn:    loop .r
.ok:
        ; DMA ch1 auto-init
        mov al, 05h
        out 0Ah, al
        out 0Ch, al
        mov al, 59h
        out 0Bh, al
        mov ax, ds
        mov dx, ax
        shl ax, 4
        shr dx, 12
        add ax, buf
        adc dx, 0
        out 02h, al
        mov al, ah
        out 02h, al
        mov al, dl
        out 83h, al
        mov ax, BUFLEN-1
        out 03h, al
        mov al, ah
        out 03h, al
        mov al, 01h
        out 0Ah, al

        in al, 21h              ; unmask IRQ5
        and al, 0DFh
        out 21h, al

        mov al, 0D1h            ; speaker on
        call dsp_write
        mov al, 40h
        call dsp_write
        mov al, 0A5h            ; ~11 kHz
        call dsp_write
        mov al, 48h
        call dsp_write
        mov al, (BUFLEN/2-1) & 0FFh
        call dsp_write
        mov al, (BUFLEN/2-1) >> 8
        call dsp_write
        mov al, 1Ch
        call dsp_write

        mov si, msg_go
        call puts
        sti
.wait:  cmp byte [done], 0
        je .wait

        mov al, 0D0h
        call dsp_write
        mov al, 0DAh
        call dsp_write
        mov al, 0D0h
        call dsp_write
        mov al, 05h
        out 0Ah, al
        push ds
        lds dx, [old9]
        mov ax, 2509h
        int 21h
        pop ds
        push ds
        lds dx, [oldd]
        mov ax, 250Dh
        int 21h
        pop ds

        mov si, msg_l
        call puts
        mov ax, [lmake]
        call putw
        mov al, '/'
        call putc
        mov ax, [lbrk]
        call putw
        mov si, msg_r
        call puts
        mov ax, [rmake]
        call putw
        mov al, '/'
        call putc
        mov ax, [rbrk]
        call putw
        mov si, msg_sb
        call puts
        mov ax, [sbirq]
        call putw
        mov si, msg_nl
        call puts
        mov ax, 4C00h
        int 21h

kb_isr: push ax
        in al, 60h
        cmp al, 0E0h
        jne .n
        mov byte [cs:e0], 1
        jmp .eoi
.n:     cmp al, 01h
        jne .x
        mov byte [cs:done], 1
.x:     cmp byte [cs:e0], 0
        je .eoi
        mov byte [cs:e0], 0
        cmp al, 4Bh
        jne .1
        inc word [cs:lmake]
.1:     cmp al, 0CBh
        jne .2
        inc word [cs:lbrk]
.2:     cmp al, 4Dh
        jne .3
        inc word [cs:rmake]
.3:     cmp al, 0CDh
        jne .eoi
        inc word [cs:rbrk]
.eoi:   mov al, 20h
        out 20h, al
        pop ax
        iret

sb_isr: push ax
        push cx
        push dx
        mov dx, BASE+0Eh
        in al, dx
        inc word [cs:sbirq]
        sti                     ; keyboard may come in here
        mov cx, 2000
.l:     loop .l
        cli
        cmp byte [cs:noeoi], 0
        jne .s
        mov al, 20h
        out 20h, al
.s:     pop dx
        pop cx
        pop ax
        iret

dsp_write:
        push ax
        push cx
        push dx
        mov ah, al
        mov dx, BASE+0Ch
        mov cx, 0FFFFh
.w:     in al, dx
        test al, 80h
        jz .g
        loop .w
.g:     mov al, ah
        out dx, al
        pop dx
        pop cx
        pop ax
        ret

puts:   lodsb
        or al, al
        jz .e
        call putc
        jmp puts
.e:     ret

putw:   push ax
        mov al, ah
        call puthex
        pop ax
puthex: push ax
        push cx
        mov ah, al
        mov cl, 4
        shr al, cl
        call .nib
        mov al, ah
        and al, 0Fh
        call .nib
        pop cx
        pop ax
        ret
.nib:   add al, '0'
        cmp al, '9'
        jbe .p
        add al, 7
.p:     jmp putc

putc:   push ax
        push bx
        push dx
        mov ah, 0Eh
        mov bx, 7
        int 10h
        pop dx
        pop bx
        pop ax
        push ax
        push dx
        mov ah, al
        mov dx, 2FDh
.w:     in al, dx
        test al, 20h
        jz .w
        mov al, ah
        mov dx, 2F8h
        out dx, al
        pop dx
        pop ax
        ret

msg_go  db "KBTEST: press arrows, ESC ends", 13, 10, 0
msg_l   db "left make/break ", 0
msg_r   db "  right make/break ", 0
msg_sb  db "  SB IRQs ", 0
msg_nl  db 13, 10, 0
old9    dd 0
oldd    dd 0
e0      db 0
done    db 0
noeoi   db 0
lmake   dw 0
lbrk    dw 0
rmake   dw 0
rbrk    dw 0
sbirq   dw 0
buf:
