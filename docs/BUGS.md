# Bug: `chessui` si blocca dopo N mosse

**Stato: RISOLTO (con conferme parziali da hardware) + seconda tornata di
fix dopo i test su hardware.**
Ultimo aggiornamento: sessione di correzione 2.

Questa nota raccoglie i sintomi, l'analisi statica del codice e le ipotesi
sul guasto.

---

## 0-bis. Report da hardware (sessione 2) e nuove correzioni

### Sintomi osservati su hardware reale

1. Il blocco originario non compare più (fix della sezione 0 confermati).
2. Primo falso allarme: mosse rifiutate — era **scacco**: con il re sotto
   scacco resta una sola mossa legale, le altre sono correttamente rifiutate
   (`ILLEGAL`). Comportamento corretto.
3. Dopo alcune mosse successive: **schermo blu con READY senza cursore**,
   cioè il programma era **uscito del tutto** (tornato a BASIC).

### Analisi

`READY` su schermo blu = BASIC dopo la fine del programma. I percorsi che
portano lì senza spiegazione erano:

- **Perdita di byte C64→ESP32 (causa scatenante più probabile):** dopo
  `STA $DE00` il C64 ripollava `$DF00` dopo ~25 µs; se l'ISR dell'ESP32
  tarda (latenza interrupt, cache miss), il C64 vedeva ancora D7 alto
  ("pronto") e scriveva il byte successivo prima che il precedente fosse
  catturato → byte perso/comandì uniti → l'ESP32 non risponde → timeout
  lato C64 → uscita.
- **`start_new_game()` non ricostruiva la board locale:** dopo un resync la
  UI restava sulla posizione vecchia mentre l'ESP32 ripartiva da quella
  iniziale → desync permanente della copia display.
- **Uscite silenziose:** fallimenti di tx/ack nel menu e a fine partita
  riportavano a BASIC senza messaggio; a fine partita il programma usciva
  sempre invece di proporre una nuova partita.
- **Risposta ignota in `game_loop`:** un byte sfasato veniva ignorato in
  silenzio (nessun ramo dell'if corrispondeva) e la partita proseguiva
  desincronizzata.

### Correzioni (sessione 2)

- `c64/comm.s`: `comm_tx_byte` ora **attende l'ack** (l'ISR dell'ESP32
  abbassa D7 quando ha catturato il byte, finestra garantita dalla durata
  dello shiftIn) con timeout ~50 ms; ritorna `0`/`-1`. Senza l'ack la gara
  sopra non è evitabile via software.
- `c64/chessui.c`: ogni invio controlla l'esito del tx; qualunque risposta
  ignota/assente è trattata come errore di collegamento (niente più
  percorsi silenziosi).
- **Nuovi comandi di protocollo** (`protocol.h` su entrambi i lati,
  `docs/PROTOCOL.md`): `GET_HIST` (`0x47`) → risposta `HIST` (`0x04`: n lo,
  n hi, n mosse lo/hi) e `GO` (`0x45`: muovi se è il turno del computer).
  La procedura `resync_board()` della UI: drain dei byte in volo, reinvio
  `SET_COLOR`/`SET_LEVEL` (copre anche un reboot dell'ESP32), `GET_HIST` e
  ricostruzione di board locale e storico, `GO` se tocca al computer.
  **La partita non si perde più dopo un errore di collegamento.**
- `esp32/esp32.ino`: `handleMove` risponde `ILLEGAL` anche se un parametro
  va in timeout (prima taceva e il C64 attendeva); gestori `GET_HIST`/`GO`.
- Fine partita: `show_game_over` chiede `N = new game, Q = quit` invece di
  uscire a BASIC.

### Da verificare su hardware

- Se lo schermo blu con READY ricompare senza nessun messaggio, sospettare
  un **reboot dell'ESP32** (che resetta anche il C64 via `oC64RST`): in quel
  caso collegare il monitor seriale USB e leggere il motivo (panic/watchdog).

---

## 0. Correzioni applicate (sessione 1)

### Timeout + resync in `comm_rx_byte` (sezione 2, H4)

`c64/comm.s`, `c64/comm.h`, `c64/chessui.c`:

- `comm_rx_byte` ora ha firma `int comm_rx_byte(unsigned ms)` (0 = attesa
  infinita): attende al più `ms` millisecondi usando il **byte basso** del
  jiffy clock del KERNAL (`$A0`, incrementa ogni jiffy = 1/60 s), poi
  **svuota il ring buffer** e ritorna `-1` invece di bloccare il C64 per
  sempre. (La prima versione leggeva `$A1`, il byte centrale, che cambia
  solo ogni 256 jiffy: il timeout risultava ~256× troppo lungo. Il byte
  basso del clock è `$A0`; `$A2` è quello alto e cambia ogni ~18 minuti.)
- Nuovo `comm_rx_flush()` per scartare i byte residui prima di inviare un
  comando (le letture tornano allineate al prossimo messaggio).
- La UI gestisce i timeout: `wait_ack`, `recv_best_payload`, `read_over_tail`
  e `game_loop` riconoscono `-1`, mostrano `LINK ERROR` e provano a
  risincronizzare con una `NEW_GAME` (`start_new_game()`), che riporta
  entrambi i lati a una posizione nota. Il timeout per la prima risposta di
  un comando è 10 s (think fino a 6 s su EXPERT), 2 s per i byte successivi
  della stessa raffica.

### H1 — NMI spurio accodato come byte valido

`c64/comm.s`: l'handler NMI legge `$DD0D` e controlla il **bit 4 (FLAG)**:
se attivo l'NMI viene dal CIA2 (tasto RESTORE) e `$DF00` NON viene letto né
accodato. Solo gli NMI della cartuccia (nessun bit CIA2) accodano il byte.
Prima un RESTORE/spurio inseriva un byte falso e sfasava il protocollo.

### H2/H3 — finestra di gara sull'handshake `io2`

`esp32/esp32.ino`: nuovo flag `nmiArm`. `isr_io2` abilita l'ack solo quando
l'ESP32 ha annunciato un byte con l'impulso `/NMI` (le letture di polling
`$DF00` del ciclo di tx del C64 generano lo stesso fronte su `/I/O2` ma non
contano più). In `busSendByte` c'è anche un margine di 100 µs dopo l'ack,
per lasciare completare la copia all'handler NMI prima del byte successivo.

### H5 — `gamePly` a 8 bit (bug latente confermato)

`esp32/chess.c`, `esp32/chess.h`: `gamePly` è promosso a `u16`, così la
soglia di compattazione `MAXKEYS - MAXSEARCH - 1` (303) diventa raggiungibile
e la compattazione parte invece di lasciare il wrap a 256 semimosse che
rompeva `repetition_count()` nelle partite lunghe. Verificato su host con un
test a 700 semimosse pseudo-random sotto ASan: compattazione a soglia 303,
nessuno sforamento di `keyHist[320]`, `repetition_count()` mai > 2.

### Da fare ancora (su hardware)

- Contatori di protocollo (`tx#`, `rx#`, `nmi#`, `overflow#`) e conferma su
  hardware che il blocco sia sparito (punti 1, 4 e 6 del piano diagnostico).
- Sync/len/checksum nel protocollo (punto 5): dopo un timeout la UI ora si
  risincronizza con `NEW_GAME`, ma un errore a metà payload ancora fa
  ripartire la partita invece di essere recuperato al volo.

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

Dopo le correzioni (sezione 0) l'handler accoda `$DF00` solo per gli NMI
della cartuccia, e `comm_rx_byte` ha un timeout con flush/resync.

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

---

## 0-quater. Report da hardware (sessione 5) e correzioni

### Sintomi osservati su hardware reale

1. La scritta "SCACCO!" **non compareva** quando era il re dell'umano a essere
   sotto scacco (compariva solo quando era la mossa dell'umano a dare scacco).
2. Partita vinta dall'AI (posizione finale di matto) chiusa invece con
   "PATTA" per 50 mosse / ripetizione.
3. Premendo `N` a fine partita si ripartiva **senza** ripassare dal menu
   (livello e colore delle impostazioni precedenti).
4. Richiesta funzionali: cronologia delle mosse e tornare indietro (undo).

### Analisi e correzioni

1. **SCACCO non mostrato:** in `recv_best_payload` il flag `F_CHECK` veniva
   copiato in `checkFlag` **prima** di `ui_apply_move()`, che azzera
   `checkFlag` (e blanca `CHECK_ROW`): l'avviso veniva cancellato subito.
   Ora i flag di protocollo vengono applicati **dopo** la mossa.
2. **Falso "PATTA":** verificato con test host che matto, stallo, 50 mosse e
   ripetizione sono rilevati correttamente e che il matto ha la priorità
   (nessun falso positivo di ripetizione su 2000 partite casuali con
   confronto completo dello stato). La causa era che le patte *reclamabili*
   (50 halfmove, triplica ripetizione) **chiudevano la partita in automatico**:
   a livello FIDE sono reclamabili, non automatiche (automatiche sono 75
   halfmove e la ripetizione 5 volte). Ora `gameOverReason()` chiude la
   partita solo per matto/stallo/materiale/75 halfmove/ripetizione 5x; le
   condizioni reclamabili arrivano come flag `F_DRAW50`/`F_DRAWREP` (in
   `BEST_MOVE` e `MOVED`) e la UI le mostra come avviso senza chiudere.
3. **Menu su `N`:** `game_loop` ritorna al chiamante con 1 = "nuova partita":
   `main` riparte dal menu (livello e colore richiesti ogni volta) invece di
   chiamare `start_new_game()` con le impostazioni correnti. `Q` esce sempre.
4. **Cronologia + undo:** pannello "MOSSE" (righe 12..19) con le ultime 14
   semimosse in coppie `12.e2e4 e7e5`; `u` al prompt annulla l'ultimo turno
   (già supportato dal protocollo con `CMD_UNDO`).

### Verifiche

- Build cc65 pulita, BSS a `$48B2` (limite `$8400`).
- Test host `/tmp/testend.c`: matto del contadino → reason=1/vincitore nero;
  50 halfmove → reason=3 esattamente a 100; 3 giri di cavalli →
  repetition_count=3; 2000 partite casuali → nessun falso positivo di
  ripetizione (verifica per confronto completo dello stato).

---

## 0-quinquies. Blocco all'inizio partita (sessione 6)

### Sintomo

Con l'ultima UI il C64 si blocca appena scelta la partita (dopo il menu).

### Analisi

Il sospetto sul timeout su `$A0` non regge: `$A0` e' il byte basso del jiffy
clock e ora i timeout scattano ai valori giusti (prima, con `$A1`, erano
256x troppo lunghi; con `$A2` del commit intermedio, 65536x). I punti che
POSSEVANO bloccare all'avvio erano due:

1. **`comm_tx_byte` attendeva il "pronto" (D7 alto) SENZA timeout**: se
   l'ESP32 non alza mai D7 (firmware senza `-DUSE_C64_BUS`, non riflashato,
   bloccato o assente) il C64 si pianta alla prima trasmissione, cioe'
   subito dopo il menu, senza alcun messaggio. Corretto: attesa del pronto
   limitata a ~58 ms; se scadde, `comm_tx_byte` ritorna -1 e la UI mostra
   LINK ERROR invece di bloccarsi.
2. La versione intermedia `$A2`: un timeout di 100 ms diventava ~ore, quindi
   `drain_link()` a inizio partita sembrava un freeze totale. Gia' corretto
   in `$A0`.

### Verifiche

- Build cc65 pulita (BSS `$48B2`).
- `esp32.ino` verificato alla sintassi con stub Arduino/gpio (`-DUSE_C64_BUS`).

### Conferma da hardware (stesso giorno)

Con il .prg "last-working" (`$A1`) la partita parte ma solo **~30 secondi**
dopo la scelta del colore: e' esattamente il timeout di `drain_link()` —
`comm_rx_byte(100)` = 6+1 tick x 256 jiffy (4,27 s ciascuno con `$A1`) =
~30 s. L'attesa c'era sempre stata (avvio "lento"); con `$A2` diventava
~6,5 ore (freeze); con `$A0` (branch attuale) sono 100 ms.

## Sessione 8 — Freeze permanente con V7 confermata a schermo

### Sintomo

Con la build V7 (branch tip, `$A0`) il gioco si blocca: il C64 disegna la
scacchiera con i pezzi due volte (normale: menu + `start_new_game`) e poi
freeza, senza LINK ERROR, senza "MOSSE" e senza "move:".

### Analisi

Con V7 ogni attesa del percorso di avvio ha un timeout:

- `drain_link()` = `comm_rx_byte(100)` → ~100 ms;
- `comm_tx_byte` → ~58 ms (pronto) + ~50 ms (ack), loop contati, indipendenti
  da `$A0`;
- `wait_ack()` → `comm_rx_byte(10000)` → ~10 s.

Un freeze permanente silenzioso non dovrebbe quindi essere possibile mentre
il jiffy clock gira. Restano due ipotesi:

1. **jiffy clock/IRQ congelato**: `comm_rx_byte` gira su `$A0` (`beq @wait`
   infinito se `$A0` non cambia); cc65 `clock()` chiama il KERNAL `RDTIM`
   ($FFDE), quindi si congelerbbero insieme timer e debounce del joystick.
   Contraddizione: il menu (cgetc) funziona e richiede l'IRQ.
2. **crash della CPU** (salto in codice corrotto): schermo congelato con
   l'ultimo contenuto scritto.

### Tracer (build V8)

Lettere `A`-`H` su MSG_ROW (riga 21), scritte man mano; al freeze la lettera
piu' avanzata indica l'ultimo punto raggiunto:

- `A` inizio `start_new_game`, prima di `drain_link`;
- `B` dopo `drain_link`;
- `C` dopo tx `NEW_GAME`;
- `D` ACK ricevuto / `E` ACK NON ricevuto;
- `F` dopo `show_history`;
- `G` ingresso in `game_loop`;
- `H` ingresso in `recv_engine_move` (solo se tocchi al nero).

Letture chiave: solo `A` → freeze dentro `drain_link` (sospettare `$A0`
congelato); `A`+`B` → dentro tx NEW_GAME (ESP32 non alza D7); fino a `D`/`E`
senza `F` → impossibile con timeout funzionanti; `F`/`G` visibili ma input
morto → IRQ morto in game_loop.
