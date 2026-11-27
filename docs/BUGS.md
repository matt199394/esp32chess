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
  jiffy clock del KERNAL, poi **svuota il ring buffer** e ritorna `-1` invece
  di bloccare il C64 per sempre. Il byte basso è **`$A2`**: sul C64 il jiffy
  clock è memorizzato **big-endian** (`UDTIM` a `$F69B` fa `inc $a2` e
  propaga il riporto su `$A1` e poi `$A0`), quindi `$A1` cambia ogni 256
  jiffy (4,27 s) e `$A0` ogni 65536 (18,2 min). **Corretto in `$A2` dopo
  essere passato per `$A1` e `$A0`** (ogni errore rendeva il timeout 256× o
  65536× troppo lungo, cioè di fatto mai): vedi "Sessione 10" in fondo e
  `c64/test_timeout.c` / `make test-timeout`.
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

Il sospetto sul timeout **non** reggeva, ma per il motivo opposto a quello
scritto allora: il byte usato (`$A0`) non è il byte basso del jiffy clock,
quindi il timeout non scattava affatto (vedi "Sessione 10"). I punti che
POSSEVANO bloccare all'avvio erano due:

1. **`comm_tx_byte` attendeva il "pronto" (D7 alto) SENZA timeout**: se
   l'ESP32 non alza mai D7 (firmware senza `-DUSE_C64_BUS`, non riflashato,
   bloccato o assente) il C64 si pianta alla prima trasmissione, cioe'
   subito dopo il menu, senza alcun messaggio. Corretto: attesa del pronto
   limitata a ~58 ms; se scadde, `comm_tx_byte` ritorna -1 e la UI mostra
   LINK ERROR invece di bloccarsi.
2. La versione intermedia `$A2`: si **credeva** che un timeout di 100 ms
   diventasse ~ore. Era il contrario: `$A2` e' il byte giusto (vedi
   "Sessione 10"); il freeze vero lo dava `$A0`, dove il timeout non
   scattava affatto.

### Verifiche

- Build cc65 pulita (BSS `$48B2`).
- `esp32.ino` verificato alla sintassi con stub Arduino/gpio (`-DUSE_C64_BUS`).

### Conferma da hardware (stesso giorno)

Con il .prg "last-working" (`$A1`) la partita parte ma solo **~30 secondi**
dopo la scelta del colore: e' esattamente il timeout di `drain_link()` —
`comm_rx_byte(100)` = 6+1 tick x 256 jiffy (4,27 s ciascuno con `$A1`) =
~30 s. Questa e' la prova che il byte corretto e' **piu' fine di `$A1`**:
con `$A2` (1 jiffy per tick) sono i ~100 ms previsti. La lettura "con `$A2`
diventava ~6,5 ore" era un'inferenza, mai misurata, e ha portato a
scegliere `$A0`, dove il timeout invece non scatta (vedi Sessione 10).

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

### Esito tracer (V8): freeze dentro drain_link (solo "A" a schermo)

Con V8 il freeze lascia **solo la lettera A**: bloccato in `drain_link()`, con
buffer rx (quasi sicuramente) vuoto e timeout su `$A0` scattante. Due sole
spiegazioni possibili: jiffy clock/IRQ congelato, oppure l'ESP32 trasmette
byte senza sosta (il drain non finisce mai). La V9 distingue i due casi:

- test diretto del clock prima del drain: loop a decadenza indipendente
  (~0,5 s) su `$A0`; **K** = ticchetta, **Z** = congelato (NOCLK, esce);
- `drain_link` mostra il numero di byte scaricati (x=8, MSG_ROW) e smette a
  200 con messaggio **FLOOD** (ESP32 trasmette senza sosta).

### Esito tracer (V9): NOCLK Z — jiffy clock davvero congelato

Il test K/Z e' verificato nell'assembly generato (la lettura di `$A0` NON
viene hoistata fuori dal loop, non e' un falso positivo): il jiffy clock
si congela tra il menu (dove cgetc, che dipende dall'IRQ KERNAL, funzionava)
e l'avvio partita, mentre il codice C continua a girare. Cause software
escluse nel codice C64: nessuna scrittura a CIA/vettori IRQ, ZP cc65
`$02-$1B`, stack C `$C000-$CFFF`, niente che tocchi `$DC0D/$DC0E/$DD0E`.

La V10: (1) probe P1-P5 che misurano il clock in 5 punti (a RUN, dopo setup,
dopo menu, dopo SET_COLOR/SET_LEVEL, a inizio partita) per individuare il
passo esatto in cui l'IRQ muore; (2) dump dei registri CIA (`$DC0D $DC0E
$DD0D $DD0E` su OVER_ROW) quando il clock risulta morto, per capire il
perche' (timer fermo? IRQ mascherato? FLAG pendente?); (3) `drain_link`
indipendente dal clock (poll + guardia FLOOD + ritardo a loop contato),
cosi' il freeze attuale non si verifica piu' anche prima di trovare la causa.

### Dump CIA (V10): timer A CIA1 ATTIVO ($DC0E=$01) ma $DC0D=$00

`$DC0E=$01` (timer A avviato, continuo) con `$DC0D=$00` (niente di latched
alla lettura) e clock fermo: il timer hardware gira ma l'IRQ non viene piu'
servito. Combinazione coerente con **mask CIA1 disabilitato** (scrittura su
`$DC0D`) oppure **vettore IRQ `$0314` alterato** — non con un guasto timer.

La V11: (1) dump esteso su tre righe: A = `$DC0D $DC0E $DC04 $DC05`,
B = `$DD0D $DD0E $0314 $0315` (stock IRQ vector = `31 EA`), D = `$0318
$0319 $01`; (2) auto-riparo: al rilevamento di clock morto, `RESTOR`
($FF87) + `IOINIT` ($FF84), reinstallazione banco VIC + `$D018` + handler
NMI (`comm_install`), nuovo probe; se torna K il gioco prosegue (marcatore
H = healed).

> **Smentita (Sessione 10)**: l'IRQ non era morto e il clock non era
> congelato. `$A0` e' il byte **alto** del jiffy clock (cambia ogni ~18
> minuti), quindi il probe K/Z diceva "Z" a ragione ma la conclusione era
> sbagliata: il clock gira, e' il byte osservato a non muoversi. Il freeze in
> `drain_link` era il timeout di `comm_rx_byte` su `$A0`, cioe' ~2 ore.
> Di conseguenza l'auto-riparo `RESTOR`/`IOINIT` della V11 (HEAL) non serve
> piu' e resta spento: con `$A2` il probe dice sempre K.

## Sessione 10 — Causa vera: il jiffy clock e' BIG-ENDIAN ($A2 = byte basso)

### Sintomo

Il timeout di `comm_rx_byte()` non scattava mai (o dopo ore): `drain_link()`
a inizio partita sembrava un freeze, il probe K/Z diceva "clock congelato",
e in VICE un test che chiama `comm_rx_byte(100)` a buffer vuoto restava
bloccato per sempre.

### Causa

Sul C64 il jiffy clock a `$A0-$A2` e' memorizzato **big-endian**: il byte che
cambia 60 volte al secondo e' **`$A2`**, non `$A0`. Fonte: il KERNAL, routine
`UDTIM` a `$F69B`:

```
F69B  INC $A2     ; 1 jiffy  = 1/60 s   <-- LSB
F69F  BNE ...
F6A1  INC $A1     ; ogni 256 jiffy = 4,27 s
F6A3  BNE ...
F6A5  INC $A0     ; ogni 65536 jiffy = 18,2 min  <-- MSB
```

Misura diretta in VICE (`x64sc`, KERNAL standard), contando i cambi dei tre
byte durante 120 IRQ (~2 s):

```
$A0: 0 cambi    $A1: 0 cambi    $A2: 119 cambi
```

Il codice invece conteggiava i cambi di `$A0`: 1 tick ogni ~18 minuti, quindi
`comm_rx_byte(100)` (~7 tick) = ~2 ore, e `comm_rx_byte(10000)` = giorni. Da
qui "il clock e' congelato" e il freeze a inizio partita. Nota che `clock()`
di cc65 usa `RDTIM` ($FFDE), che gestisce i tre byte correttamente: per
questo i timer a schermo funzionavano mentre il timeout no.

Storia dei byte (tutti i tentativi): `$A1` -> timeout 256x troppo lungo
(hardware: partita che iniziava dopo ~30 s), `$A0` -> 65536x troppo lungo
(freeze), `$A2` -> corretto.

### Fix

- `c64/comm.s`: `JIF_LO = $a2` (costante con il commento sul big-endian) al
  posto di `lda $a0`; corretto il commento.
- `c64/chessui.c`: `clock_probe()` ora legge `$A2` (con `$A0` diceva sempre
  "NOCLK"/Z).
- `c64/test_timeout.c` + `make test-timeout`: regressione eseguibile in VICE
  headless (bordo verde = ok). Il test misura i tick consumati da
  `comm_rx_byte()` a ring buffer vuoto sui casi 100/500/2000/10000 ms.
- `docs/BUGS.md`: corrette le conclusioni delle sessioni 6-8.

### Esito del test (VICE, dopo il fix)

```
  100 ms -> -1 in   7 tick (attesi 7..9)
  500 ms -> -1 in  32 tick (attesi 32..34)
 2000 ms -> -1 in 126 tick (attesi 126..128)
10000 ms -> -1 in 627 tick (attesi 626..628)
TUTTI OK
```

Con `$A0` lo stesso test restava bloccato sul primo caso (solo il banner a
schermo): la regressione vede il bug.

---

## Sessione 11 — Avvio: 9 s di attesa, causa `drain_link` "a loop contato"

### Sintomo (hardware, build V11 + fix `$A2`)

Con il fix `$A2` il tracer mostra **A** poi **K** (clock vivo, niente
HEAL/NOCLK) e la partita parte, ma dopo **~9 secondi**.

### Causa

`drain_link()` ritardava con un loop contato (`2 x 40000` iterazioni di
`drain_sink = PEEK(0x01)`), un workaround scritto quando si credeva il jiffy
clock congelato. Misurato in VICE con lo stesso codice: **429 jiffies =
7150 ms**. Sommati a probe, `draw_board()` e trasferimento si arriva ai ~9 s
osservati. Non era colpa del link: era attesa morta.

### Fix

`drain_link()` ora misura l'attesa sul jiffy clock (`clock()` = `RDTIM`):
`DRAIN_GAP_JFY = 3` jiffy (~50 ms) per passata, due passate = **~100 ms**;
`DRAIN_GUARD = 20000` poll come tetto nel caso (non piu' atteso) di clock
fermo, cosi' non si puo' tornare a un'attesa illimitata.

Misura in VICE dello stesso codice: **6 jiffies = 100 ms** (era 429).

### Pulizia (V12)

Con la causa vera chiusa, rimossa la strumentazione che serviva alla caccia:
lettere tracer A-H su `MSG_ROW`, `dump_cia()`/`dump_line()`,
`kernal_reinit()` (HEAL/NOCLK) e le probe del clock P1-P5. Il marcatore di
build a schermo passa a **V12** (i `.prg` precedenti si riconoscono cosi').
`drain_link()` resta quello nuovo (deadline sul clock + `DRAIN_GUARD`).

---

## Sessione 12 — Flash di scacchiera malformata a RUN

### Sintomo

Appena RUN, per una frazione di secondo appare una scacchiera malformata,
poi la schermata definitiva col menu. Assente nelle prime versioni.

### Causa

In `main()` la commutazione video (`$DD00` banca 2 + `$D018` schermo `$8400`,
charset `$8800`) avveniva PRIMA di `install_charset()` e del primo
`draw_board()`: nel lasso tra flip e primo clear/disegno (una frazione di
secondo) il VIC mostrava la memoria non ancora inizializzata, che al re-RUN
contiene i resti della scacchiera precedente. La finestra si era allungata
con le probe V10-V11 inserite in mezzo.

### Fix

Ordine invertito: `install_charset()` e il primo `draw_board()` (menu,
emptyBoard=1) preparano `$8400`/`$8800` mentre il VIC mostra ancora lo
schermo del BASIC (le scritture sono invisibili), POI flip di `$DD00`/`$D018`:
il menu appare gia' completo, senza flash. Al primo giro del menu il
ridisegno viene saltato (variabile `first`).

---

## Sessione 13 — Grafica pezzi "Don's Gambit" (charset multicolor)

### Obiettivo

Usare la grafica dei pezzi di *The Don's Gambit* (`dongambit.prg` +
`dongambit.png`, riferimento aggiunto al branch) al posto dei glyph hires
"solid/outline".

### Analisi

- Screenshot 1:1 (finestra VICE): scacchiera 192x192 px a (40,35),
  24 px/casa = 3x3 caratteri, come il nostro layout.
- Per ogni casa ci sono SOLO 2 colori (casa + pezzo); i pezzi sono
  riempimenti SOLIDI (bianco su casa per i bianchi, nero su casa per i
  neri), stessa forma per i due colori.
- La vista e' ruotata (lato nero in basso, file h->a): la corona a 3 punte
  e' la REGINA (d1), la forma con palla+corona e' il RE (e1). Torre a
  a/h, cavallo b/g (con occhio e orecchio), alfiere c/f (mitra con taglio),
  pedoni sulla seconda riga.
- La ricerca dei dati raw nel .prg (hires, multicolor charset e bitmap,
  sprite 24x21, flip/byte-reverse) non trova corrispondenze: i pezzi non
  sono memorizzati come bitmap raw (probabilmente compressi o procedurali).
  Non serve: il PNG e' una fonte pixel-perfect, i glyph si estraggono da li'.

### Scelta tecnica: charset multicolor

Il bianco SOLIDO su casa colorata esige il multicolor (in hires il pezzo
bianco puo' essere solo contorno su $D021 nero). Modalita': `$D016` bit 4
globale, con lo switch per carattere dato dal **bit 3 del color RAM**:

- coppie `01` -> `$D022` = bianco (pezzi bianchi), `10` -> `$D023` = nero
  (pezzi neri), `11` -> color RAM = colore casa;
- caratteri con color RAM 0-7 restano HIRES: tutto il testo del pannello
  resta normale, a 8 px, senza font riscritto. Unico vincolo: i colori del
  TESTO devono stare in 0-7 (LIGHTRED -> RED per gli avvisi).

### Cambi (V13)

- `glyph_mc[2][6][9][8]` (864 B, generati da `dongambit.png`) sostituisce
  `glyph_solid`/`glyph_outline`: bianchi da glyph 0x40, neri da 0x76,
  9 caratteri per pezzo; blocco casa vuota spostato da 0xA0 a 0xE0
  (tutte le coppie 11 = colore casa).
- `$D016` bit 4 + `$D022` bianco + `$D023` nero al flip video di `main()`.
- Colori casa (ora vincolati a 0-7): GIALLO/BLU al posto di GRAY3/BLU
  (GRAY3 = 15 non e' rappresentabile come colore casa multicolor);
  ultima mossa/selezione: CIANO (era giallo, ora coincide con le case
  chiare); cursore joystick: VERDE (era ciano).
- Marcatore di build: **V13**.

### Verifica

- Round-trip dei glyph contro il PNG (maschera quantizzata multicolor): OK.
- Anteprima resa dalla tabella glyph: 12 forme riconoscibili, nessun
  artefatto (dettagli da 1 px diventano da 2 px, limite fisico del MC).
- Build cc65 senza warning; RODATA+BSS finiscono a $4B2F, molto sotto
  $8400 (schermo).

### Aggiornamento: multicolor rimosso (V14)

Su richiesta dell'utente il multicolor e' stato tolto. Le FORME dei pezzi
"Don's Gambit" restano (stesso layout: neri da glyph 0x40, bianchi da 0x76,
9 caratteri per pezzo, casa vuota a 0xE0), ma tornano in charset HIRES:

- neri = riempimento solido esatto (bit 0 = pezzo su $D021 nero);
- bianchi = contorno di 1 px del pezzo (il bianco pieno su casa colorata
  in hires non e' rappresentabile); valutate anteprime "contorno puro"
  vs "contorno + retino": il contorno puro e' risultato il piu' leggibile;
- colori originali ripristinati: case GRAY3/BLU, ultima mossa GIALLO,
  cursore CIANO, avvisi LIGHTRED, niente $D016/$D022/$D023.
- Marcatore di build: **V14**. Build senza warning, BSS fine $4B1C.

---

## Sessione 14 — LINK ERROR dopo ~20 mosse

### Sintomo (hardware, build V14)

A grafica approvata, dopo circa 20 mosse di gioco compare **LINK ERROR** a
partita in corso.

### Analisi

Due debolezze residue del trasporto, la prima determinante:

1. **`busSendByte` (ESP32) abbandonava il byte in silenzio**: se l'ack I/O2
   non arrivava entro 50 ms, il firmware passava comunque al byte
   successivo senza ripetere l'impulso /NMI. Il C64 non vedrebbe MAI quel
   byte: stream desincronizzato, il primo byte "strano" o il timeout di
   `comm_rx_byte` producevano LINK ERROR. Un singolo ack perso in tutta la
   partita bastava.
2. **Lato C64 ogni anomalia era visibile**: i punti di errore del game loop
   facevano `link_error()` (messaggio + attesa di un tasto) PRIMA di
   `resync_board()`, anche quando il resync silenzioso avrebbe riparato
   tutto senza interrompere la partita.

Il meccanismo di perdita plausibile: un impulso /NMI perso o un fronte
/I/O2 mancato (ISR lenta, rumore, races a ~µs) diventavano un byte perso
perché il lato ESP32 non aveva nessun tentativo successivo.

### Fix (V15)

- `esp32/esp32.ino`: `busSendByte` **ripete l'impulso /NMI fino a 5 volte**
  (20 ms per tentativo, ~100 ms totali) prima di dichiarare il byte perso.
  Ripetere e' sicuro: il byte resta su D0..D7 e se il C64 avesse gia'
  letto $DF00 l'ack sarebbe arrivato (niente doppia consegna). Su fallimento
  finale c'e' un messaggio su Serial (debug).
- `c64/chessui.c`: nuova `recover_link()`: prova un **resync silenzioso**
  (drain + colore/livello + GET_HIST + ricostruzione board) e mostra
  LINK ERROR solo se anche il resync fallisce. Usata in `do_undo` e in tutti
  i punti di errore del game loop (send_move fallita, flag MOVED perso,
  payload BEST_MOVE interrotto, risposta ignota). Un byte perso in volo ora
  si ripara senza schermo blu e senza premere tasti.
- Marcatore di build: **V15**.

### Verifica

- Build cc65 senza warning; BSS totale $46D, RODATA $6A5 (sotto $8400).
- `g++ -fsyntax-only -std=gnu++11 -DUSE_C64_BUS` su `esp32.ino`: OK.
- Su hardware: l'utente deve **riflashare l'ESP32** (il retry dell'impulso
  NMI e' lato firmware) e ricaricare il .prg V15; poi partita lunga di test.
