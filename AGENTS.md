# AGENTS.md — esp32chess (bozza, sessione 25 nov)

Gioco di scacchi **umano vs computer**: il **motore gira sull'ESP32** (cartuccia
c64esp32cart), il **C64 fa solo da UI** (scacchiera, tastiera, testo, suoni).

> Specifica di riferimento: `~/lavoro/esp43chess/AGENTS.md` (architettura,
> hardware, protocollo) e `~/lavoro/chess2026/AGENTS.md` (motore CHESS 2026,
> codifica mosse, board 0x88, UI C64). Leggerli prima di toccare qualcosa.

## Stato: prima bozza funzionante in compilazione

- **FATTO** porting motore (`chess.c`/`chess.h` di CHESS 2026) sull'ESP32.
- **FATTO** protocollo §4 implementato su entrambi i lati.
- **FATTO** trasporto ESP32: `Serial` (default, per test) + bus cartuccia
  (`-DUSE_C64_BUS`).
- **FATTO** trasporto C64: `comm.s` (tx polling + rx via NMI, ring buffer 16B).
- **FATTO** UI C64 **grafica** 3×3-charset (da chess-alphabeta): scacchiera 24×24,
  pezzi glyph invertiti, pannello sinistro, effetti sonori SID, joystick port 2,
  doppio timer (giocatore/AI), mossa del giocatore mostrata subito (snapshot/restore),
  avviso "SCACCO!" (rosso = sei sotto scacco, giallo = hai dato scacco),
  cronologia mosse a schermo + undo con `u`, fine partita con N = menu principale
  (livello+colore riprodotti ogni volta) e Q = esci.
- **NOTA** patte reclamabili (50 halfmove, triplica ripetizione) NON chiudono
  la partita: arrivano come flag `F_DRAW50`/`F_DRAWREP` e sono mostrate come
  avviso; chiudono solo matto/stallo/materiale/75 halfmove/ripetizione 5x.
- **TODO** vedere "Prossimi passi" in fondo.

## Build (verificati in questa sessione)

```bash
# C64 UI -> chessui.prg
cd c64 && make

# regressione del timeout di comm_rx_byte (VICE headless, bordo verde = ok)
cd c64 && make test-timeout

# ESP32 motore (seriale)
arduino-cli compile --fqbn esp32:esp32:mhetesp32minikit esp32
arduino-cli upload  -p /dev/ttyACM0 --fqbn esp32:esp32:mhetesp32minikit esp32

# ESP32 motore (bus cartuccia)
arduino-cli compile --fqbn esp32:esp32:mhetesp32minikit \
  --build-property "build.extra_flags=-DUSE_C64_BUS" esp32

# prova motore via seriale
python3 tools/uart_test.py /dev/ttyACM0          # umano bianco
python3 tools/uart_test.py /dev/ttyACM0 black    # umano nero
```

Entrambi i target compilano puliti (nessun warning).

## Struttura

```
esp32/
  esp32.ino        sketch: trasporto + sessione + protocollo
  chess.c/chess.h  motore (copiato da chess2026, non modificare senza perft)
  protocol.h       costanti protocollo (KEEP IN SYNC con c64/protocol.h)
c64/
  chessui.c        UI grafica 3×3-charset + macchina a stati
  font_writer.h    font 512B per il charset custom
  comm.s/comm.h    byte a byte $DE00/$DF00 + NMI ($0318, ring buffer 16B)
  protocol.h       costanti protocollo
  Makefile         cl65 -> chessui.prg
  test_timeout.c   regressione timeout comm_rx_byte (VICE)
docs/PROTOCOL.md   contratto del protocollo + trasporto fisico
tools/uart_test.py prova motore su seriale
tools/vice_test_timeout.py  esegue test_timeout.prg in x64sc, controlla il bordo
```

## Decisioni prese in questa sessione (da non ribaltare senza motivo)

1. **Trasporto a 8 bit pieni**, NON a 7 bit:
   - C64 → ESP32: polling + flow control (`$DF00` bit 7 = "pronto", poi `STA $DE00`).
   - ESP32 → C64: byte su `$DF00` + impulso `/NMI`; il C64 legge nel handler NMI.
   - Quindi mossa (16 bit) e score (s16) viaggiano come **2 byte grezzi lo/hi**.
2. **Handler NMI utente installato al vettore RAM `$0318`**: il KERNAL a `$FE43`
   fa SOLO `sei` + `jmp ($0318)` **senza** pushati i registri (il default di
   `$0318` punta a `$FE47`, l'handler RESTORE, che li pusha lui). Quindi
   l'handler in `comm.s` salva/ripristina A, X, Y da solo (come `+pushreg`/`+popreg`
   della cartuccia di riferimento); P lo ripristina `rti`. Attenzione: NON fare
   `pla/tay, pla/tax, pla` all'ingresso: spillerebbe i 3 byte (PCL,PCH,P) pushata
   dall'NMI e `rti` leggerebbe spazzatura -> crash del C64 (sintomo: READY).
3. **Ring buffer rx da 16 byte (15 utili)** in `comm.s`: assorbe la raffica
   `BEST_MOVE` (7 byte) + `GAME_OVER` (3) senza perdite.
4. **ESP32 = unico arbitro**; la UI C64 mantiene solo una copia della board per
   il display (`apply_move_local`, gestisce arrocco/en passant/promozione solo
   per disegnare) e NON decide la legalità.
5. **Livelli motore** (draft, da rivedere): easy d2/0s, medium d4/1s, hard d6/3s,
   expert d8/6s. `quiesceMax = 4`.
6. **UNDO** ricostruisce la partita dall'inizio riapplicando le mosse tranne le
   ultime due (sia ESP32 sia C64, con storico locale). Semplice e corretto.

## Insidie già pagate (non reintrodurle)

1. Arduino richiede che il `.ino` abbia lo **stesso nome della cartella**:
   `esp32/esp32.ino` (non `esp32chess.ino`).
2. Core ESP32 3.x: `gpio_set_level()` vuole `gpio_num_t` → serve il cast
   `(gpio_num_t)busD[i]`, altrimenti non compila.
3. Su cc65 `histN` dev'essere `unsigned int` (non `unsigned char`): il check
   `histN < 400` con `unsigned char` è costante (warning + buffer overflow
   dopo 256 semimosse).
4. `chess.c` ha `__fastcall__` solo dentro `#ifdef USE_ASM_MOVEGEN`: NON
   definire `USE_ASM_MOVEGEN` su ESP32 (il movegen C è quello giusto).
5. Promozione a cavallo = indice 0: usare `is_promo()`, mai `M_PROMO(m) != 0`.
6. **Boot del C64 = schermo nero**: in `USE_C64_BUS` il `linkSetup()` DEVE
   pilotare `GPIO21` (`oC64RST`) HIGH e `GPIO2` (`CLED`/`/EXROM`) LOW, poi
   fare un impulso di reset. Senza questi pin il C64 resta in reset (GPIO21
   flottante → `/RESET` basso via 74LS14) e non si avvia. Il bus dati D0-D7
   NON c'entra: è isolato dal 74LS244 (abilitato solo su lettura `$DF00`,
   net `IO2` → `U3-1`/`U3-19`). Vedi schema `c64esp32cart/c64esp32crt1.1_kicad7`.
7. **Il jiffy clock del C64 è BIG-ENDIAN**: il byte che cambia 60 volte al
   secondo è `$A2` (`UDTIM` a `$F69B`: `inc $a2` → carry su `$A1` → `$A0`);
   `$A1` cambia ogni 4,27 s, `$A0` ogni 18,2 min. Ogni timeout implementato
   "a mano" (es. `comm_rx_byte`) deve leggere **`$A2`**: con `$A1`/`$A0` il
   timeout diventa 256×/65536× più lungo, cioè un freeze silenzioso. Questo
   errore è già costato un bug lungo (vedi "Sessione 10" in `docs/BUGS.md`).
   Regressione: `make test-timeout` in `c64/` (VICE headless).

## Prossimi passi (ordine suggerito)

1. ~~Porting motore + protocollo + UI testuale~~ FATTO (bozza).
2. Provare il motore con `tools/uart_test.py` (e un paio di mosse: `e2e4`,
   promozione `e7e8q`, mossa illegale).
3. ~~Portare la UI grafica C64 3×3-charset~~ FATTO (grafica, SID, joystick,
   timer, mossa immediata; font in `c64/font_writer.h`).
4. Mock dell'ESP32 (o modalità simulata) per provare la UI in VICE senza
   cartuccia.
5. Transposition table + history heuristic sull'ESP32 (RAM/CPU abbondanti).
6. `SEARCH_INFO` periodico per nodi/depth durante il "penso…".
7. Undo robusto, promozione con scelta, conferma uscita.
8. Test su hardware reale: handshake `$DF00` bit 7, rimbalzo joystick, timing
   `/NMI` (150 µs).

## Regole per chi modifica

1. La **legalità è solo dell'ESP32**: la UI non deve fidarsi della propria board.
2. Codifica mossa (16 bit) e board 0x88 sono **condivise**: modifiche insieme su
   `esp32/` e `c64/` + `protocol.h` su entrambi i lati.
3. `docs/PROTOCOL.md` è il contratto: estendere senza rompere i comandi esistenti.
4. Se tocchi `chess.c`/`chess.h`, valida con il perft di CHESS 2026 prima.
5. Prima di ogni commit: `make` in `c64/` e i due `arduino-cli compile` sopra.
