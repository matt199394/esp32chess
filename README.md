# esp32chess — scacchi C64 con motore su ESP32

Gioco di scacchi **umano vs computer** per Commodore 64 con una architettura
insolita: **il motore gira sull'ESP32**, dentro la cartuccia `c64esp32cart`,
mentre **il C64 fa solo da interfaccia** (scacchiera, input da tastiera, testo,
in futuro grafica e suoni).

L'ESP32 è l'**unico arbitro** della partita: tiene la posizione 0x88, le regole,
la valutazione e la ricerca alpha-beta. Il C64 mantiene soltanto una **copia
della board per il disegno** e dialoga con l'ESP32 attraverso un protocollo
binario a 8 bit pieni sul bus della cartuccia (`$DE00` / `$DF00` + `/NMI`).

> **Stato attuale: bozza funzionante ma NON stabile.** Il motore, il protocollo
> e la UI testuale compilano e girano; resta però un **bug aperto e non
> risolto**: `chessui` (la UI C64) **si blocca dopo un numero di mosse che si
> ripete partita dopo partita**. Dettagli e analisi in
> [Stato dello sviluppo](#stato-dello-sviluppo) e [`docs/BUGS.md`](docs/BUGS.md).

---

## Indice

- [Architettura](#architettura)
- [Hardware e pinout della cartuccia](#hardware-e-pinout-della-cartuccia)
- [Protocollo](#protocollo)
- [Build e prova](#build-e-prova)
- [Stato dello sviluppo](#stato-dello-sviluppo)
- [Struttura del repository](#struttura-del-repository)
- [Roadmap](#roadmap)
- [Riferimenti](#riferimenti)

---

## Architettura

```
┌──────────────────────────────┐        bus cartuccia         ┌──────────────────────────┐
│  Commodore 64                │  $DE00 (C64 → ESP32)         │  ESP32 (WROOM-32)        │
│                              │  $DF00 (ESP32 → C64)         │                          │
│  chessui.prg (cc65)          │  /NMI, /I/O1, /I/O2          │  chess.c / chess.h       │
│   - board di display         │  D0..D7, /RESET, /EXROM      │   - board 0x88           │
│   - cursore / input          │ ◄──────────────────────────► │   - movegen legale       │
│   - macchina a stati         │                              │   - alpha-beta + qsearch │
│   - NESSUNA legalità         │                              │   - zobrist / ripetizioni│
└──────────────────────────────┘                              └──────────────────────────┘
```

- **ESP32**: `esp32/esp32.ino` + motore portato da **CHESS 2026**
  (`esp32/chess.c`, `esp32/chess.h`, C89, nessun I/O, nessuna allocazione
  dinamica). Trasporto selezionabile in compilazione:
  - default → `Serial` 115200 (comodo per provare il motore da PC);
  - `-DUSE_C64_BUS` → bus reale della cartuccia.
- **C64**: `c64/chessui.c` (UI testuale con `conio`, da sostituire con la
  scacchiera grafica 3×3-charset di CHESS 2026) + `c64/comm.s` (trasporto a
  byte e handler NMI).

### Flusso di gioco

1. Il C64 chiede colore e livello, poi invia `SET_COLOR`, `SET_LEVEL`,
   `NEW_GAME`.
2. Se l'umano è nero, l'ESP32 (bianco) apre subito con `BEST_MOVE`.
3. L'umano invia `MOVE`; l'ESP32 valida, applica, pensa e risponde con
   `BEST_MOVE` (oppure `ILLEGAL` / `GAME_OVER`).
4. L'ESP32 controlla anche le regole di patta (50 mosse, tripla ripetizione,
   materiale insufficiente).

---

## Hardware e pinout della cartuccia

Scheda: **MH ET LIVE ESP32MiniKit / ESP32-WROOM-32**
(FQBN `esp32:esp32:mhetesp32minikit`).
Hardware condiviso con il progetto [`c64esp32cart`](https://github.com/matt199394/c64esp32cart)
(schema KiCad `c64esp32crt1.1_kicad7`).

La cartuccia usa il **connettore di espansione del C64** (edge connector,
22 pin sul lato superiore + 22 sul lato inferiore) con:

- un **74LS244** che porta verso il C64 i dati scritti dall'ESP32
  (`$DF00` in lettura);
- un **74HC597** (shift register a caricamento parallelo) che cattura il byte
  scritto dal C64 su `$DE00` e lo serializza verso l'ESP32;
- **74LS14** (Schmitt trigger) per `/NMI` e `/RESET`;
- un **BC547** che pilota `/EXROM` dal GPIO2;
- eventuale **27C512** e microSD (non usate da esp32chess).

### ESP32 → segnali della cartuccia

| GPIO ESP32 | Dir. | Segnale | Pin connettore C64 | Note |
|---:|---|---|---:|---|
| 4  | out | D0 | 21 | bus dati ESP32 → C64 (74LS244) |
| 33 | out | D1 | 20 | |
| 14 | out | D2 | 19 | |
| 27 | out | D3 | 18 | |
| 13 | out | D4 | 17 | |
| 22 | out | D5 | 16 | |
| 17 | out | D6 | 15 | |
| 26 | out | D7 | 14 | **anche flow-control**: alto = ESP32 pronto a ricevere |
| 32 | out | `/NMI` | D | impulso ~150 µs per consegnare un byte al C64 |
| 25 | out | `SCLK` | — | clock dello shift register 74HC597 |
| 16 | out | `/PL` (pload) | — | parallel load dello shift register |
| 21 | out | `/RESET` C64 | C | via buffer; alto = C64 in run |
| 2  | out | `/EXROM` (CLED) | 9 | via BC547: **alto = EXROM basso = EPROM mappata** |
| 34 | in  | `SDATA` | — | uscita seriale del 74HC597 (C64 → ESP32) |
| 35 | in  | `/I/O1` | 7 | interrupt: il C64 ha scritto `$DE00` |
| 36 | in  | `/I/O2` | 10 | interrupt: il C64 ha letto `$DF00` |
| 15 | in  | pulsante reset | — | net `iRST`, **non** collegato al `/RESET` del C64 |

### Segnali del connettore di espansione usati

| Pin | Segnale | Uso in esp32chess |
|---:|---|---|
| 1, 22, A, Z | GND | massa |
| 2, 3 | +5 V | alimentazione |
| 4 | `/IRQ` | non usato |
| 5 | `R/W` | qualifica accessi |
| 6 | `DOT Clk` (PHI0) | non usato |
| 7 | `/I/O1` | **`$DE00`** — C64 scrive i comandi verso l'ESP32 |
| 8 | `/GAME` | jumper, tenuto alto |
| 9 | `/EXROM` | pilotato dal BC547 (GPIO2) |
| 10 | `/I/O2` | **`$DF00`** — C64 legge le risposte dall'ESP32 |
| 11 | `/ROML` | jumper ROM (non usato qui) |
| 12 | `BA` | non usato |
| 13 | `/DMA` | non usato |
| 14–21 | D7…D0 | bus dati a 8 bit |
| B | `/ROMH` | jumper ROM |
| C | `/RESET` | pilotato da GPIO21 (run/reset del C64) |
| D | `/NMI` | pilotato da GPIO32, consegna byte al C64 |
| E | `PHI2` | qualifica accessi |
| F–Y | A15…A0 | non usati direttamente (decodifica `/I/O1`, `/I/O2` nel C64) |

> **Nota sul boot (già pagata):** in modalità `USE_C64_BUS` il firmware deve
> pilotare `GPIO21` (reset C64) **alto** e `GPIO2` (`/EXROM`) **basso**, poi
> dare un impulso di reset. Senza questi pin il C64 resta in reset (schermo
> nero). Il bus dati non c'entra: è isolato dal 74LS244, abilitato solo in
> lettura di `$DF00`.

---

## Protocollo

Contratto completo in [`docs/PROTOCOL.md`](docs/PROTOCOL.md). Sintesi:

### Trasporto fisico (8 bit pieni)

| Direzione | Meccanismo |
|---|---|
| **C64 → ESP32** | polling: il C64 attende `$DF00` bit 7 = 1 (ESP32 pronto), poi `STA $DE00`. L'ISR `/I/O1` dell'ESP32 abbassa D7 appena riceve il byte e lo rilegge dallo shift register. |
| **ESP32 → C64** | l'ESP32 mette il byte su D0..D7, abbassa `/NMI` per ~150 µs e attende che il C64 legga `$DF00` (ISR `/I/O2`). L'handler NMI del C64 legge `$DF00` e accoda il byte in un **ring buffer da 16 byte**. |

Non c'è alcun bit di parità né imbottitura a 7 bit: **mossa e score viaggiano
come 2 byte grezzi lo/hi**.

### Comandi C64 → ESP32

| Comando | Codice | Parametri | Significato |
|---|---|---|---|
| `NEW_GAME`  | `0x4E` `'N'` | — | nuova partita |
| `SET_COLOR` | `0x43` `'C'` | 1 byte (0 bianco, 1 nero) | lato dell'umano |
| `SET_LEVEL` | `0x4C` `'L'` | 1 byte (0..3) | difficoltà |
| `MOVE`      | `0x4D` `'M'` | mossa lo, hi | mossa dell'umano |
| `UNDO`      | `0x55` `'U'` | — | ritira l'ultima coppia di mosse |
| `QUIT`      | `0x51` `'Q'` | — | termina / torna al menu |

### Risposte ESP32 → C64

| Codice | Nome | Payload |
|---|---|---|
| `0x00` | `ILLEGAL` | — |
| `0x01` | `ACK` | — |
| `0x02` | `BEST_MOVE` | mossa lo, mossa hi, score lo, score hi, depth (u8), flags (u8) |
| `0x03` | `GAME_OVER` | reason (u8), winner (u8) |

`flags`: bit 0 = `F_CHECK`, bit 1 = `F_GAMEOVER` (segue subito `GAME_OVER`).
`reason`: 1 matto, 2 stallo, 3 regola 50 mosse, 4 tripla ripetizione,
5 materiale insufficiente. `winner`: 0 bianco, 1 nero, `0xFF` patta.

### Codifica mossa (16 bit, identica su entrambi i lati)

```
bit  0..6   from (indice 0x88)
bit  7..13  to   (indice 0x88)
bit 14..15  promozione: 0 = N, 1 = B, 2 = R, 3 = Q
MOVE_NONE = 0xFFFF
```

> La promozione a **cavallo** ha indice 0: usare sempre `is_promo()` (pedone
> che arriva sull'ultima traversa), **mai** `M_PROMO(m) != 0`.
>
> Byte sul filo: `lo = m & 0xFF`, `hi = (m >> 8) & 0xFF`.

---

## Build e prova

### C64 (UI)

```bash
cd c64
make                 # -> chessui.prg
x64sc chessui.prg    # solo UI: senza cartuccia resta in attesa di $DF00 bit 7
```

### ESP32 (motore via seriale, per test)

```bash
arduino-cli compile --fqbn esp32:esp32:mhetesp32minikit esp32
arduino-cli upload  -p /dev/ttyACM0 --fqbn esp32:esp32:mhetesp32minikit esp32

# prova automatica del protocollo sulla USB (pyserial)
python3 tools/uart_test.py /dev/ttyACM0          # umano bianco
python3 tools/uart_test.py /dev/ttyACM0 black    # umano nero
```

### ESP32 (bus cartuccia)

```bash
arduino-cli compile --fqbn esp32:esp32:mhetesp32minikit \
  --build-property "build.extra_flags=-DUSE_C64_BUS" esp32
arduino-cli upload  -p /dev/ttyACM0 --fqbn esp32:esp32:mhetesp32minikit esp32
```

> In VICE, senza cartuccia, `comm_tx_byte` resta in attesa di `$DF00` bit 7:
> per provare solo la UI serve un mock dell'ESP32 (TODO) oppure l'hardware
> reale.

---

## Stato dello sviluppo

### Fatto e verificato in compilazione

- [x] Porting del motore CHESS 2026 su ESP32 (board 0x88, movegen, alpha-beta,
      quiescenza, zobrist, regole di patta).
- [x] Protocollo §4 implementato su entrambi i lati.
- [x] Trasporto ESP32: `Serial` (default) e bus cartuccia (`-DUSE_C64_BUS`).
- [x] Trasporto C64: `comm.s` (tx in polling + rx via NMI, ring buffer 16 byte).
- [x] UI C64 testuale con `conio`: board, input `e2e4`, undo, promozione,
      gestione arrocco/en passant **solo per il disegno**.
- [x] Boot del C64 pilotato dall'ESP32 (`/RESET` + `/EXROM`).
- [x] Entrambi i target compilano puliti.

### In corso / da fare

- [ ] UI grafica C64 3×3-charset (porting da `chess2026/src/main_c64.c`).
- [ ] Mock ESP32 per provare la UI in VICE senza cartuccia.
- [ ] Transposition table / history heuristic sull'ESP32.
- [ ] `SEARCH_INFO` periodico durante il "penso…".
- [ ] Test sistematici su hardware reale.

### 🐞 BUG APERTO (non risolto): `chessui` si blocca dopo un numero fisso di mosse

**Sintomo.** La UI C64 si **blocca** (nessun input, nessun aggiornamento)
dopo un certo numero di mosse. Il numero di mosse a cui avviene il blocco
**si ripete partita dopo partita**: rigiocando la stessa sequenza il blocco
arriva nello stesso punto. L'ESP32 continua a girare (il firmware ha timeout
interni e non si blocca), ma il C64 resta fermo in attesa di un byte che non
arriva più.

**Perché è un blocco definitivo.** In `c64/comm.s` la funzione
`comm_rx_byte` è un'attesa **senza timeout**:

```asm
_comm_rx_byte:
        lda _comm_rx_head
        cmp _comm_rx_tail
        beq _comm_rx_byte      ; buffer vuoto -> attende per sempre
        ...
```

Basta **un solo byte perso o fuori sequenza** perché il C64 aspetti
eternamente. L'ESP32, al contrario, ha timeout (50 ms in `busSendByte`,
2000 ms in `linkReadByte`): dopo un errore abbandona il trasferimento e
continua, quindi la sequenza resta **definitivamente disallineata**. È il
meccanismo che trasforma un glitch transitorio in un blocco permanente.

**Analisi del codice / ipotesi in esame** (nessuna ancora confermata sul
campo):

1. **Perdita di un byte nella raffica `BEST_MOVE` + `GAME_OVER`.**
   L'ESP32 invia fino a 7 byte (`BEST_MOVE`) + 3 (`GAME_OVER`) consecutivi,
   sincronizzati solo dall'impulso `/NMI` e dall'ack `/I/O2`. Il ring buffer
   da 16 byte (15 utili) dovrebbe bastare, ma se un NMI viene perso o
   l'handshake si accavalla il conteggio va fuori fase.
2. **NMI spurio che accoda spazzatura.** L'handler NMI legge `$DF00` e lo
   accoda **sempre**, anche se il byte non è stato richiesto dall'ESP32 (es.
   NMI da `CIA2`/RESTORE o rumore sulla linea). Un byte in più sposta tutta
   la lettura successiva di una posizione → `comm_rx_byte` attende un byte
   che non esiste più.
3. **Letture di `$DF00` fuori dalla ricezione.** Anche il ciclo di polling di
   `comm_tx_byte` (`lda $df00` / `bpl`) genera transizioni su `/I/O2`, quindi
   interrupt `isr_io2` sull'ESP32. Se una di queste letture cade tra
   `io2 = false` e l'impulso `/NMI` di `busSendByte`, l'ESP32 può credere che
   il byte sia già stato consumato e sovrascrivere il successivo.
4. **Nessun byte di sincronizzazione / nessun controllo di sequenza.** Il
   protocollo non ha header, lunghezza o checksum: dopo il primo
   disallineamento non c'è modo di recuperare (manca un resync).
5. **Bug latente nel motore, probabilmente non la causa del blocco.**
   `gamePly` è `u8` (0..255) ma `commit_move()` compatta `keyHist` con la
   soglia `(u8)(MAXKEYS - MAXSEARCH - 1) = 303` (non raggiungibile da un
   `u8`): la compattazione non parte mai e `gamePly` va in wrap dopo 256
   semimosse. Non provoca crash (gli indici restano dentro `keyHist[320]`),
   ma **rompe il rilevamento delle ripetizioni** a partita lunga. Va corretto
   comunque.

**Cosa è stato escluso / verificato.** Compilazione pulita di entrambi i
target; nessun overflow dimostrabile del ring buffer con le raffiche previste
(≤10 byte su 15 utili); nessun crash dell'ESP32 (i timeout sono lato
firmware); `gameHist`/`hist` locali non traboccano alle lunghezze osservate.

**Prossimi passi diagnostici (proposta).**

- Log seriale lato ESP32 di **ogni** byte inviato/ricevuto con contatore
  (`tx#`, `rx#`) e confronto con il contatore lato C64.
- Aggiungere al C64 un **timeout** in `comm_rx_byte` con contatore di
  overflow del ring buffer, per capire se il blocco è "buffer vuoto" o
  "buffer pieno".
- Introdurre un **byte di sync/len** (o un resync su timeout) nel protocollo.
- Disabilitare temporaneamente l'ack `/I/O2` e misurare; poi reintrodurlo con
  attesa esplicita per-byte.
- Verificare con l'oscilloscopio/logica analyzer il timing `/NMI` ↔ lettura
  `$DF00` sui casi che si bloccano.

Dettaglio esteso e note di analisi: [`docs/BUGS.md`](docs/BUGS.md).

---

## Struttura del repository

```
esp32/
  esp32.ino        sketch Arduino: trasporto (Serial / bus) + sessione + protocollo
  chess.c          motore (porting di CHESS 2026; non modificare senza perft)
  chess.h
  protocol.h       costanti del protocollo (KEEP IN SYNC con c64/protocol.h)
c64/
  chessui.c        UI testuale + macchina a stati (bozza)
  comm.s           trasporto byte a byte $DE00/$DF00 + handler NMI
  comm.h
  protocol.h
  Makefile         cl65 -> chessui.prg
docs/
  PROTOCOL.md      contratto del protocollo + trasporto fisico
  BUGS.md          analisi del bug aperto
tools/
  uart_test.py     prova il motore via seriale
```

---

## Roadmap

1. ~~Porting motore + protocollo + UI testuale~~ (fatto).
2. **Risolvere il blocco di `chessui`** (priorità: vedi [`docs/BUGS.md`](docs/BUGS.md)).
3. UI grafica C64 3×3-charset (scacchiera 24×24, pezzi glyph 64–123,
   pannello sinistro, joystick, suoni SID) riusando `chess2026/src/main_c64.c`
   e mantenendo `comm.s` al posto del motore locale.
4. Mock dell'ESP32 per provare la UI in VICE senza cartuccia.
5. Transposition table + history heuristic sull'ESP32.
6. `SEARCH_INFO` periodico (nodi/depth) durante la ricerca.
7. Undo robusto, promozione con scelta, conferma uscita.
8. Test su hardware reale: handshake `$DF00` bit 7, rimbalzo joystick,
   timing `/NMI` (150 µs).

---

## Riferimenti

- Specifica di progetto: `~/lavoro/esp43chess/AGENTS.md` (architettura,
  hardware, protocollo) e `~/lavoro/chess2026/AGENTS.md` (motore CHESS 2026,
  codifica mosse, board 0x88, UI C64).
- Cartuccia e schema: <https://github.com/matt199394/c64esp32cart>
  (KiCad `c64esp32crt1.1_kicad7`, schematic, gerber).
- Ispirazione hardware: chat64 cart di Bart Venneker e Theo van den Beld.

## Licenza

Codice del motore derivato da **CHESS 2026**; il resto è materiale del
progetto. Aggiungere qui la licenza definitiva prima della pubblicazione
(es. MIT/GPL) — attualmente **non specificata**.
