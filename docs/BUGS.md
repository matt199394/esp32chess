# Bug aperto: `chessui` si blocca dopo N mosse

**Stato: APERTO — non risolto.**
Ultimo aggiornamento: sessione corrente (bozza).

Questa nota raccoglie i sintomi, l'analisi statica del codice e le ipotesi
sul guasto. Non contiene ancora una causa confermata né un rimedio.

---

## 1. Sintomo

- La UI C64 (`chessui.prg`) **si blocca**: il programma non accetta più
  input, lo schermo non si aggiorna, nessun messaggio.
- Il blocco avviene **dopo un numero di mosse che si ripete partita dopo
  partita**: rigiocando la stessa sequenza il blocco arriva nello stesso
  punto.
- L'ESP32 **continua a girare**: il firmware ha timeout interni
  (`busSendByte` 50 ms, `linkReadByte` 2000 ms) e non resta appeso.
- Il C64 torna operativo solo con reset (pulsante cartuccia / RESET).

Il sintomo è compatibile con un **disallineamento del protocollo**: il C64
attende un byte che l'ESP32 non invierà più, oppure l'ESP32 ha già consumato
byte che il C64 considera ancora da leggere.

---

## 2. Punto critico: `comm_rx_byte` non ha timeout

`c64/comm.s`:

```asm
_comm_rx_byte:
        lda _comm_rx_head
        cmp _comm_rx_tail
        beq _comm_rx_byte      ; buffer vuoto -> attende per sempre
        ldx _comm_rx_tail
        lda _comm_rx_buf,x
        ...
```

Il consumatore (main loop) è un'attesa **illimitata**. Qualunque byte perso,
duplicato o fuori sequenza congela il C64 in modo permanente.

Lato ESP32, invece, ogni trasferimento ha un timeout e **abbandona** il
byte mancante. È esattamente la combinazione che rende definitivo un errore
transitorio:

1. si perde/duplica un byte;
2. l'ESP32 va avanti dopo il timeout;
3. il C64 resta in `comm_rx_byte` per sempre (o legge payload sbagliati);
4. nessun resync è previsto dal protocollo.

---

## 3. Il trasporto in breve

### C64 → ESP32

```asm
_comm_tx_byte:
        sta _comm_tx_tmp
@w:     lda $df00
        bpl @w              ; attende bit 7 alto = ESP32 pronto
        lda _comm_tx_tmp
        sta $de00
        rts
```

- Ogni `lda $df00` del ciclo di polling genera una transizione su `/I/O2`
  (pin 10 del connettore).
- Sull'ESP32 `/I/O2` è collegato a `GPIO36` con interrupt sul fronte di
  discesa (`isr_io2`), che mette `io2 = true`.

### ESP32 → C64 (`esp32.ino`)

```cpp
static void busSendByte(byte b) {
  outByte(b);                 // D0..D7
  io2 = false;                // <-- azzera l'ack
  digitalWrite(oC64NMI, HIGH);
  delayMicroseconds(150);
  digitalWrite(oC64NMI, LOW); // impulso (asserzione via 74LS14)
  while (!io2) {              // attende che il C64 legga $DF00
    if (micros() - t0 > 50000) break;   // timeout 50 ms
    delayMicroseconds(10);
  }
  io2 = false;
}
```

### Handler NMI (`c64/comm.s`)

```asm
comm_nmi:
        bit $dd0d              ; ack CIA2 (RESTORE), evita NMI spuri
        php / pha / txa pha / tya pha
        ldx _comm_rx_head
        lda _comm_rx_head
        clc / adc #1 / and #15
        cmp _comm_rx_tail
        beq @full              ; pieno: legge e scarta
        sta _comm_rx_head      ; aggiorna head PRIMA di leggere $df00
        lda $df00
        sta _comm_rx_buf,x
        jmp @done
@full:  lda $df00              ; handshake comunque
@done:  pla tay / pla tax / pla / plp / rti
```

Ring buffer: `_comm_rx_buf` da 16 byte, indici a 4 bit → **15 byte utili**.

---

## 4. Ipotesi (in ordine di probabilità percepita)

### H1 — NMI spurio accodato come byte valido

L'handler legge `$DF00` e lo accoda **sempre**. Se arriva un NMI non
generato dall'ESP32 (RESTORE via CIA2, rumore sulla linea, rimbalzo),
l'handler inserisce nel ring buffer un byte che il main loop consumerà al
posto del successivo vero byte di protocollo. Da quel momento la lettura è
sfasata di uno: `recv_best_payload` legge mossa/score/depth/flags sbagliati,
poi `comm_rx_byte` attende un byte che l'ESP32 non manderà più.

Il `bit $dd0d` iniziale **riconosce** l'NMI del CIA2 ma non evita che venga
accodato `$DF00` (che in quel momento non contiene dati validi).

**Come verificarlo:** contatore di NMI nel main loop, o flag "sto
aspettando un byte" controllato dall'handler (se non atteso, ignora/ack senza
accodare).

### H2 — Raffica troppo veloce / byte sovrascritto

`BEST_MOVE` è 7 byte, più `GAME_OVER` 3 = 10 byte consecutivi. Il buffer da
15 utili dovrebbe assorbirli, ma la raffica è sincronizzata solo da
`/NMI` + `io2`. Se:

- un impulso `/NMI` cade mentre `io2` è ancora `true` per una lettura di
  polling (`comm_tx_byte`), oppure
- l'ESP32 azzera `io2 = false` dopo che il C64 ha già letto ma prima che
  l'handler abbia finito,

il conteggio si sfasa e l'ESP32 può sovrascrivere il byte successivo prima
che l'handler lo abbia copiato nel buffer.

**Come verificarlo:** log `tx#/rx#` su entrambi i lati, contatore di
overflow/full nel ring buffer.

### H3 — `/I/O2` generato anche dal polling `$DF00` in tx

Il ciclo `@w: lda $df00; bpl @w` legge `$DF00` ripetutamente. Ogni lettura
genera un fronte su `/I/O2` → `isr_io2` → `io2 = true`. Quando l'ESP32
chiama `busSendByte`, fa `io2 = false`, ma una lettura di polling del C64
può riportarlo a `true` **prima** dell'impulso `/NMI`, facendo credere
all'ESP32 che il byte sia già stato consumato.

La finestra è stretta ma reale, e spiegherebbe un errore **dipendente dal
percorso** (stesse mosse → stesse tempistiche → stesso punto di blocco).

**Come verificarlo:** isolare `/I/O2` dall'handshake (usare solo `/NMI`, o un
contatore di sequenza) e vedere se il blocco scompare.

### H4 — Mancanza di sync/lunghezza/checksum nel protocollo

Non esiste un byte di sincronizzazione né una lunghezza: dopo un qualsiasi
errore non c'è modo di riallinearsi. Anche se H1–H3 fossero rari, il primo
evento provoca un blocco permanente.

**Miglioria:** header con contatore o `LEN`, e un **timeout con resync** in
`comm_rx_byte` (es. dopo N ms svuota il buffer e torna a uno stato noto).

### H5 (bug latente, probabilmente NON la causa)

`esp32/chess.c` / `esp32/chess.h`:

```c
u8  gamePly;                                   /* 0..255 */
...
if (gamePly >= (u8)(MAXKEYS - MAXSEARCH - 1)) { /* 320-16-1 = 303 */
    for (i = 0; i < 64; i++) keyHist[i] = keyHist[gamePly - 63 + i];
    gamePly = 63;
}
```

La soglia `303` non è raggiungibile da un `u8`: la compattazione **non parte
mai** e `gamePly` va in wrap dopo 256 semimosse. Gli indici restano dentro
`keyHist[320]` (max `255 + 15 = 270`), quindi **non c'è crash**, ma
`repetition_count()` usa una base sbagliata dopo il wrap e il rilevamento
della tripla ripetizione si rompe a partita lunga.

Va corretto (portare `gamePly` a `u16` o usare una soglia compatibile), ma
da solo non spiega un blocco del C64.

---

## 5. Cosa è stato escluso

- **Compilazione**: entrambi i target compilano puliti (cc65 `-Oirs`,
  arduino-cli mhetesp32minikit).
- **Overflow del ring buffer con le raffiche previste**: 10 byte massimi
  contro 15 utili.
- **Crash dell'ESP32**: il firmware ha timeout e continua; il blocco è solo
  lato C64.
- **Overflow di `hist[400]` / `gameHist[400]`** alle lunghezze osservate.

---

## 6. Piano diagnostico proposto

1. **Contatori di protocollo** su entrambi i lati: `tx#`, `rx#`, `nmi#`,
   `overflow#`. Confrontarli al momento del blocco.
2. **Timeout + resync** in `comm_rx_byte`: se il buffer resta vuoto oltre
   N ms, svuota e segnala errore invece di attendere per sempre.
3. **Flag "attesa byte"** nell'handler NMI: accoda `$DF00` solo se l'ESP32
   ha realmente annunciato un byte (es. `/NMI` atteso), altrimenti scarta.
4. **Isolare `/I/O2`**: testare l'handshake solo su `/NMI` o con un
   contatore di sequenza per-byte.
5. **Byte di sync/len** nel protocollo, così un errore è rilevabile e
   recuperabile.
6. **Logica analyzer / oscilloscopio** su `/NMI`, `/I/O2`, D0..D7 durante la
   sequenza che si blocca (catturare il primo byte perso).
7. Correggere in parallelo il bug latente `gamePly` (H5).

---

## 7. File coinvolti

| File | Ruolo |
|---|---|
| `c64/comm.s` | trasporto C64, handler NMI, ring buffer, `comm_rx_byte` senza timeout |
| `c64/chessui.c` | macchina a stati che consuma i byte (`wait_ack`, `recv_best_payload`) |
| `esp32/esp32.ino` | `busSendByte`, `busReadByte`, ISR `/I/O1` e `/I/O2` |
| `esp32/chess.c` | bug latente `gamePly`/`keyHist` |
| `docs/PROTOCOL.md` | contratto del protocollo |
