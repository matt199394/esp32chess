;
; comm.s - trasporto byte a byte tra C64 e ESP32 (cartuccia c64esp32cart)
;
;   C64 -> ESP32 :  attende $df00 bit 7 (ESP32 "pronto"), poi STA $de00
;   ESP32 -> C64 :  l'ESP32 mette il byte su $df00 e abbassa /NMI; il KERNAL
;                   smista gli NMI "utente" al vettore RAM $0318 (installato
;                   da comm_install). Un ring buffer da 16 byte assorbe le
;                   raffiche della risposta (BEST_MOVE = 7 byte in coda).
;
;   NB: il KERNAL a $FE43 fa SOLO `sei` + `jmp ($0318)` SENZA pushati i
;       registri (il default di $0318 punta a $FE47, l'handler RESTORE, che
;       li pusha lui). Quindi l'handler qui salva/ripristina A, X, Y da solo;
;       P viene ripristinato da rti (l'NMI lo ha pushato sullo stack).

        .export _comm_install
        .export _comm_tx_byte
        .export _comm_rx_byte

.segment "BSS"

_comm_rx_head:  .res 1          ; indice di scrittura (produttore: NMI)
_comm_rx_tail:  .res 1          ; indice di lettura  (consumatore: main)
_comm_rx_buf:   .res 16         ; ring buffer (capienza 15: assorbe BEST_MOVE 7 byte)
_comm_tx_tmp:   .res 1
_comm_rx_tmp:   .res 1

.segment "CODE"

; ---------------------------------------------------------------------------
; void comm_install(void)
; ---------------------------------------------------------------------------
_comm_install:
        sei
        lda #<comm_nmi
        sta $0318
        lda #>comm_nmi
        sta $0319
        lda #0
        sta _comm_rx_head
        sta _comm_rx_tail
        cli
        rts

; ---------------------------------------------------------------------------
; void __fastcall__ comm_tx_byte(unsigned char b)    ; b in A
; ---------------------------------------------------------------------------
_comm_tx_byte:
        sta _comm_tx_tmp
@w:     lda $df00
        bpl @w                          ; attende bit 7 alto (pronto)
        lda _comm_tx_tmp
        sta $de00
        rts

; ---------------------------------------------------------------------------
; unsigned char __fastcall__ comm_rx_byte(void)      ; ritorna il byte in A
; ---------------------------------------------------------------------------
_comm_rx_byte:
        lda _comm_rx_head
        cmp _comm_rx_tail
        beq _comm_rx_byte               ; vuoto: attende
        ldx _comm_rx_tail
        lda _comm_rx_buf,x
        sta _comm_rx_tmp
        lda _comm_rx_tail
        clc
        adc #1
        and #15
        sta _comm_rx_tail
        lda _comm_rx_tmp
        rts

; ---------------------------------------------------------------------------
; NMI utente: legge $df00 e lo accoda al ring buffer
; ---------------------------------------------------------------------------
comm_nmi:
        bit $dd0d                        ; ack CIA2 NMI (RESTORE), evita NMI spuri
        php                             ; salva P
        pha                             ; salva A
        txa
        pha                             ; salva X
        tya
        pha                             ; salva Y

        ldx _comm_rx_head               ; X = vecchio head (indice di scrittura)
        lda _comm_rx_head
        clc
        adc #1
        and #15
        cmp _comm_rx_tail
        beq @full                       ; buffer pieno: scarta il byte
        sta _comm_rx_head               ; aggiorna head
        lda $df00
        sta _comm_rx_buf,x              ; scrive nel vecchio head
        jmp @done
@full:
        lda $df00                       ; legge e scarta
@done:
        pla                             ; ripristina Y
        tay
        pla                             ; ripristina X
        tax
        pla                             ; ripristina A
        plp                             ; ripristina P
        rti
