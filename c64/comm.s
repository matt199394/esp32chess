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
;
;   L'handler NMI accoda $df00 SOLO se l'NMI arriva dalla cartuccia: un NMI
;   generato dal CIA2 (tasto RESTORE) e' riconosciuto dal bit 4 di $dd0d e
;   scartato, altrimenti inserirebbe nel ring buffer un byte falso che
;   sfaserebbe il protocollo.
;
;   comm_rx_byte ha un timeout: alla scadenza svuota il ring buffer (resync)
;   e ritorna -1 invece di bloccare il C64 per sempre.

        .export _comm_install
        .export _comm_tx_byte
        .export _comm_rx_byte
        .export _comm_rx_flush

.segment "BSS"

_comm_rx_head:  .res 1          ; indice di scrittura (produttore: NMI)
_comm_rx_tail:  .res 1          ; indice di lettura  (consumatore: main)
_comm_rx_buf:   .res 16         ; ring buffer (capienza 15: assorbe BEST_MOVE 7 byte)
_comm_tx_tmp:   .res 1
_comm_rx_tmp:   .res 1
_rx_tmo:        .res 2          ; timeout, in jiffies (1/60 s)
_rx_last:       .res 1          ; ultimo campione del jiffy clock
_rx_cnt:        .res 2          ; jiffies attesi finora

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
; int __fastcall__ comm_rx_byte(unsigned int ms)     ; timeout in AX, 0 = infinito
;   Ritorna il byte (0..255) in A/X, oppure -1 su timeout. Alla scadenza
;   svuota il ring buffer: le letture tornano allineate al prossimo
;   messaggio invece di restare sfasate per sempre.
; ---------------------------------------------------------------------------
_comm_rx_byte:
        sta _rx_tmo                     ; ms lo
        stx _rx_tmo+1                   ; ms hi
        lsr _rx_tmo+1                   ; ms -> jiffies (60 Hz): circa /16
        ror _rx_tmo
        lsr _rx_tmo+1
        ror _rx_tmo
        lsr _rx_tmo+1
        ror _rx_tmo
        lsr _rx_tmo+1
        ror _rx_tmo
        lda $a1                         ; jiffy clock del KERNAL (60 Hz)
        sta _rx_last
        lda #0
        sta _rx_cnt
        sta _rx_cnt+1
@wait:  lda _comm_rx_head
        cmp _comm_rx_tail
        bne @got
        lda _rx_tmo
        ora _rx_tmo+1
        beq @wait                       ; timeout 0: attesa infinita
        lda $a1
        cmp _rx_last
        beq @wait
        sta _rx_last
        inc _rx_cnt
        bne @chk
        inc _rx_cnt+1
@chk:   lda _rx_tmo+1
        cmp _rx_cnt+1
        bcc @tmo
        bne @wait
        lda _rx_tmo
        cmp _rx_cnt
        bcs @wait
@tmo:   jsr _comm_rx_flush
        lda #$ff
        ldx #$ff
        rts
@got:   ldx _comm_rx_tail
        lda _comm_rx_buf,x
        sta _comm_rx_tmp
        lda _comm_rx_tail
        clc
        adc #1
        and #15
        sta _comm_rx_tail
        lda _comm_rx_tmp
        ldx #0
        rts

; ---------------------------------------------------------------------------
; void comm_rx_flush(void): scarta i byte ricevuti ma non consumati
; ---------------------------------------------------------------------------
_comm_rx_flush:
        php
        sei                             ; head e' toccato dall'NMI: finestra minima
        lda _comm_rx_head
        sta _comm_rx_tail
        plp
        rts

; ---------------------------------------------------------------------------
; NMI utente: legge $df00 e lo accoda al ring buffer
; ---------------------------------------------------------------------------
comm_nmi:
        pha                             ; salva A
        lda $dd0d                       ; ack CIA2 NMI; bit 4 = FLAG = RESTORE
        and #$10
        beq @cart
        pla                             ; NMI del CIA2 (RESTORE): niente da leggere
        rti
@cart:  txa
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
        rti                             ; P ripristinato da rti
