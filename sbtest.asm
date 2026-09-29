; SBTEST.COM - Sound Blaster detection steps as games do them, with timeouts.
; Assumes A220 I5 D1. Build: nasm -f bin -o SBTEST.COM sbtest.asm
; Results go to the screen and to COM2 (so QEMU can log them).
        org 100h

BASE    equ 220h
IRQ     equ 5
VEC     equ 08h + IRQ

start:
        mov si, msg_hello
        call puts

; ---- 1. DSP reset
        mov dx, BASE+6
        mov al, 1
        out dx, al
        mov cx, 100
.d1:    in al, 80h
        loop .d1
        xor al, al
        out dx, al
        mov cx, 0FFFFh
        mov dx, BASE+0Eh
.w1:    in al, dx
        test al, 80h
        jnz .r1
        loop .w1
        mov si, msg_reset_to
        call puts
        jmp done
.r1:    mov dx, BASE+0Ah
        in al, dx
        mov si, msg_reset
        push ax
        call puts
        pop ax
        call puthex
        call crlf

; ---- 2. version
        mov al, 0E1h
        call dsp_write
        call dsp_read
        mov si, msg_ver
        push ax
        call puts
        pop ax
        call puthex
        call dsp_read
        call puthex
        call crlf

; ---- 3. busy flag ever set?
        mov cx, 0FFFFh
        mov dx, BASE+0Ch
.w3:    in al, dx
        test al, 80h
        jnz .b_ok
        loop .w3
        mov si, msg_busy_no
        call puts
        jmp .b_done
.b_ok:  mov si, msg_busy_yes
        call puts
.b_done:

; ---- hook IRQ
        cli
        xor ax, ax
        mov es, ax
        mov ax, [es:VEC*4]
        mov [old_off], ax
        mov ax, [es:VEC*4+2]
        mov [old_seg], ax
        mov word [es:VEC*4], isr
        mov [es:VEC*4+2], cs
        in al, 21h
        mov [old_mask], al
        and al, ~(1 << IRQ)
        out 21h, al
        sti

; ---- 4. F2h IRQ request
        mov byte [got_irq], 0
        mov al, 0F2h
        call dsp_write
        call wait_irq
        mov si, msg_f2
        call puts
        call put_result

        mov al, 0D1h            ; speaker on
        call dsp_write

; ---- 5. short DMA transfer (32 bytes), wait IRQ
        mov cx, 32
        call setup_dma
        mov byte [got_irq], 0
        mov al, 40h
        call dsp_write
        mov al, 0A6h
        call dsp_write
        mov al, 14h
        call dsp_write
        mov al, 31
        call dsp_write
        xor al, al
        call dsp_write
        call wait_irq
        mov si, msg_dma_short
        call puts
        call put_result

; ---- 6. 4000-byte transfer: IRQ and DMA count
        mov cx, 4000
        call setup_dma
        mov byte [got_irq], 0
        mov al, 14h
        call dsp_write
        mov ax, 3999
        call dsp_write
        mov al, ah
        call dsp_write
        ; poll DMA count until it changes from 3999 (progress) - bounded
        mov bx, 0
.pc:    out 0Ch, al
        in al, 03h
        mov ah, al
        in al, 03h
        xchg al, ah
        cmp ax, 3999
        jne .moved
        dec bx
        jnz .pc
        mov si, msg_cnt_stuck
        call puts
        jmp .cnt_done
.moved: mov si, msg_cnt_moved
        call puts
.cnt_done:
        call wait_irq_long
        mov si, msg_dma_long
        call puts
        call put_result
        ; count after completion
        out 0Ch, al
        in al, 03h
        mov ah, al
        in al, 03h
        xchg al, ah
        push ax
        mov si, msg_cnt_end
        call puts
        pop ax
        push ax
        mov al, ah
        call puthex
        pop ax
        call puthex
        call crlf

; ---- 7. auto-init: 3 IRQs from 1Ch, then DAh
        mov cx, 4000
        call setup_dma_auto
        mov byte [got_irq], 0
        mov al, 48h
        call dsp_write
        mov ax, 1999
        call dsp_write
        mov al, ah
        call dsp_write
        mov al, 1Ch
        call dsp_write
        mov bx, 55
        call wait_n3
        mov si, msg_auto
        call puts
        mov al, [got_irq]
        call puthex
        call crlf
        mov al, 0DAh
        call dsp_write

; ---- 8. AdLib timer detection at 388h
        mov ah, 4
        mov al, 60h
        call fm_write
        mov al, 80h
        call fm_write
        mov dx, 388h
        in al, dx
        mov bl, al
        mov ah, 2
        mov al, 0FFh
        call fm_write
        mov ah, 4
        mov al, 21h
        call fm_write
        mov cx, 200
.fmw:   in al, 80h
        loop .fmw
        mov dx, 388h
        in al, dx
        mov bh, al
        mov ah, 4
        mov al, 60h
        call fm_write
        mov al, 80h
        call fm_write
        mov si, msg_adlib_no
        and bx, 0E0E0h
        cmp bx, 0C000h
        jne .fmr
        mov si, msg_adlib_yes
.fmr:   call puts

; ---- restore
        cli
        xor ax, ax
        mov es, ax
        mov ax, [old_off]
        mov [es:VEC*4], ax
        mov ax, [old_seg]
        mov [es:VEC*4+2], ax
        mov al, [old_mask]
        out 21h, al
        sti
done:
        mov si, msg_bye
        call puts
        mov ax, 4C00h
        int 21h

; ------------------------------------------------------------ helpers
isr:    push ax
        push dx
        mov dx, BASE+0Eh
        in al, dx
        inc byte [cs:got_irq]
        mov al, 20h
        out 20h, al
        pop dx
        pop ax
        iret

wait_irq:                       ; up to ~0.5 s
        mov bx, 9
        jmp wait_ticks
wait_irq_long:                  ; up to ~3 s
        mov bx, 55
wait_ticks:
        push es
        xor ax, ax
        mov es, ax
        mov ax, [es:46Ch]
        add bx, ax
.l:     cmp byte [got_irq], 0
        jne .ok
        sti
        mov ax, [es:46Ch]
        cmp ax, bx
        jne .l
.ok:    pop es
        ret

wait_n3:                        ; wait until got_irq >= 3 or ~3 s
        push es
        xor ax, ax
        mov es, ax
        mov ax, [es:46Ch]
        add bx, ax
.l:     cmp byte [got_irq], 3
        jae .ok
        mov ax, [es:46Ch]
        cmp ax, bx
        jne .l
.ok:    pop es
        ret

setup_dma_auto:
        call setup_dma
        mov al, 05h
        out 0Ah, al
        mov al, 59h             ; auto-init, single, read, ch1
        out 0Bh, al
        mov al, 01h
        out 0Ah, al
        ret

fm_write:                       ; AH = register, AL = value
        push ax
        mov dx, 388h
        xchg al, ah
        out dx, al
        mov cx, 6
.a:     in al, dx
        loop .a
        inc dx
        mov al, ah
        out dx, al
        mov cx, 35
.b:     in al, dx
        loop .b
        pop ax
        ret

put_result:
        cmp byte [got_irq], 0
        je .n
        mov si, msg_yes
        jmp puts
.n:     mov si, msg_no
        jmp puts

; CX = length; DMA channel 1, single, read (memory->device) from buf
setup_dma:
        mov al, 05h             ; mask ch1
        out 0Ah, al
        out 0Ch, al
        mov al, 49h             ; single, increment, read, ch1
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
        mov ax, cx
        dec ax
        out 03h, al
        mov al, ah
        out 03h, al
        mov al, 01h             ; unmask ch1
        out 0Ah, al
        ret

dsp_write:
        push ax
        push cx
        mov dx, BASE+0Ch
        mov ah, al
        mov cx, 0FFFFh
.w:     in al, dx
        test al, 80h
        jz .go
        loop .w
.go:    mov al, ah
        out dx, al
        pop cx
        pop ax
        ret

dsp_read:
        push cx
        mov dx, BASE+0Eh
        mov cx, 0FFFFh
.w:     in al, dx
        test al, 80h
        jnz .go
        loop .w
.go:    mov dx, BASE+0Ah
        in al, dx
        pop cx
        ret

puts:   lodsb
        or al, al
        jz .e
        call putc
        jmp puts
.e:     ret

crlf:   mov al, 13
        call putc
        mov al, 10
putc:   push ax
        push dx
        mov dl, al
        mov ah, 2
        int 21h
        pop dx
        pop ax
        push ax
        push dx
        mov ah, al
        mov dx, 2FDh            ; COM2 LSR
.w:     in al, dx
        test al, 20h
        jz .w
        mov al, ah
        mov dx, 2F8h
        out dx, al
        pop dx
        pop ax
        ret

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
.p:     push ax
        call putc
        pop ax
        ret

msg_hello       db "SBTEST", 13, 10, 0
msg_reset_to    db "reset: TIMEOUT", 13, 10, 0
msg_reset       db "reset: ", 0
msg_ver         db "version: ", 0
msg_busy_yes    db "busy bit: seen", 13, 10, 0
msg_busy_no     db "busy bit: never set", 13, 10, 0
msg_f2          db "F2h IRQ: ", 0
msg_dma_short   db "32-byte DMA IRQ: ", 0
msg_cnt_moved   db "DMA count: moving", 13, 10, 0
msg_cnt_stuck   db "DMA count: STUCK", 13, 10, 0
msg_dma_long    db "4000-byte DMA IRQ: ", 0
msg_cnt_end     db "DMA count at end: ", 0
msg_auto        db "auto-init IRQs in 3 s (want 03): ", 0
msg_adlib_yes   db "AdLib: detected", 13, 10, 0
msg_adlib_no    db "AdLib: NOT detected", 13, 10, 0
msg_yes         db "yes", 13, 10, 0
msg_no          db "NO", 13, 10, 0
msg_bye         db "done", 13, 10, 0
got_irq         db 0
old_off         dw 0
old_seg         dw 0
old_mask        db 0
buf             times 4000 db 80h
