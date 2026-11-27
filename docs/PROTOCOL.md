# Protocollo di comunicazione ESP32 ↔ C64 (bozza v0)

Contratto minimo descritto in `~/lavoro/esp43chess/AGENTS.md` §4, qui precisato
per quanto riguarda il **trasporto fisico**.

## Trasporto fisico (cartuccia c64esp32cart)

Due registri, due direzioni, entrambi **a 8 bit pieni**:

| Direzione | Come | Dettaglio |
|---|---|---|
| C64 → ESP32 | polling + flow control | il C64 attende `$DF00` bit 7 = 1 (ESP32 "pronto"), poi `STA $DE00` |
| ESP32 → C64 | NMI + handshake di lettura | l'ESP32 mette il byte su `$DF00`, abbassa `/NMI`, aspetta che il C64 legga `$DF00` |

Nota importante: **niente "bit 7 = dati a 7 bit"**. Il bit 7 di `$DF00` è il
flag "pronto" solo nella direzione C64→ESP32; nell'altra direzione `$DF00`
trasporta un intero byte e la sincronizzazione è fatta con la linea `/NMI`.
Quindi **mossa e score viaggiano come 2 byte grezzi (lo, hi)** e non serve
ricodificarli a 7 bit.

### C64 → ESP32 (comando)

```
wait_for_ready:  lda $df00 / bpl wait_for_ready   ; bit7 alto = pronto
                 sta $de00                        ; scrive il byte
```

L'ISR `io1` dell'ESP32 abbassa `oC64D7` (bit 7 di `$DF00`) appena riceve il
byte, e il loop principale lo rialza quando ha finito di processarlo.

### ESP32 → C64 (risposta)

```
outByte(b)         ; byte su D0..D7
triggerNMI()       ; /NMI basso ~150us
wait io2           ; il C64 ha letto $df00
```

Il C64 installa un handler NMI **utente** al vettore RAM `$0318` (il KERNAL vi
salta per gli NMI non-RESTORE). L'handler legge `$df00` e lo accoda a un ring
buffer da 16 byte (15 utili), così le raffiche di risposta non si perdono;
gli NMI generati dal CIA2 (tasto RESTORE) vengono riconosciuti dal bit 4 di
`$dd0d` e scartati. `comm_rx_byte` ha un timeout: alla scadenza svuota il
buffer e ritorna `-1` invece di bloccare il C64.

## Messaggi

Tutti i comandi sono **1 byte + eventuali parametri**; tutte le risposte sono
**1 byte + eventuale payload**.

### C64 → ESP32

| Comando | Codice | Parametri | Significato |
|---|---|---|---|
| `NEW_GAME`  | `0x4E` 'N' | — | nuova partita da posizione iniziale |
| `SET_COLOR` | `0x43` 'C' | 1 byte: 0 bianco, 1 nero | lato dell'umano |
| `SET_LEVEL` | `0x4C` 'L' | 1 byte: 0..3 | difficoltà |
| `MOVE`      | `0x4D` 'M' | 2 byte: mossa lo, hi | mossa dell'umano |
| `UNDO`      | `0x55` 'U' | — | ritira la coppia di mosse |
| `QUIT`      | `0x51` 'Q' | — | fine / torna al menu |
| `GET_HIST`  | `0x47` 'G' | — | chiede lo storico mosse (resync) |
| `GO`        | `0x45` 'E' | — | muovi se è il turno del computer, altrimenti `ACK` |
| `FAME_LOAD` | `0x46` 'F' | 1 byte: indice partita famosa (0..29) | prepara il replay: `ACK` + n lo + n hi (n = semimosse della partita) |
| `FAME_NEXT` | `0x50` 'P' | — | applica la prossima semimossa della partita famosa: `MOVED` + flags + mossa lo + mossa hi, oppure `GAME_OVER` a lista esaurita |

### ESP32 → C64

| Codice | Nome | Payload | Significato |
|---|---|---|---|
| `0x00` | `ILLEGAL`   | — | mossa rifiutata |
| `0x01` | `ACK`       | — | conferma (`NEW_GAME`/`SET_*`/`UNDO`/`QUIT`) |
| `0x02` | `BEST_MOVE` | mossa lo, mossa hi, score lo, score hi, depth (u8), flags (u8) | risposta del computer |
| `0x03` | `GAME_OVER` | reason (u8), winner (u8) | fine partita |
| `0x04` | `HIST`      | n lo, n hi, poi n mosse (lo, hi) | storico completo della partita |
| `0x05` | `MOVED`     | flags (u8) | mossa dell'umano accettata; flag = scacco dato + patte reclamabili. Inviato subito dopo `MOVE`, prima del think; segue `BEST_MOVE` o `GAME_OVER`. Nel replay delle famose il payload è flags + mossa lo + mossa hi |

### Codifica mossa (16 bit, identica su entrambi i lati)

```
bit 0..6   from (0x88)
bit 7..13  to   (0x88)
bit 14..15 promozione: 0=N, 1=B, 2=R, 3=Q
```

- `MOVE_NONE = 0xFFFF`.
- La promozione a cavallo ha indice 0: usare `is_promo()` (pedone che arriva
  sull'ultima traversa), **mai** `M_PROMO(m) != 0`.
- Sull'ESP32 il byte lo è `m & 0xFF`, il byte hi è `(m >> 8) & 0xFF`.

### Flags di `BEST_MOVE` e `MOVED`

| Bit | Nome | Significato |
|---|---|---|
| 0 | `F_CHECK` | `BEST_MOVE`: l'umano è sotto scacco dopo la risposta del computer. `MOVED`: la mossa dell'umano dà scacco |
| 1 | `F_GAMEOVER` | solo `BEST_MOVE`: subito dopo segue un messaggio `GAME_OVER` |
| 2 | `F_DRAW50` | 50 halfmove senza pedoni/catture: patta **reclamabile**, la partita continua |
| 3 | `F_DRAWREP` | posizione ripetuta 3 volte: patta **reclamabile**, la partita continua |

### `GAME_OVER`

Chiudono la partita solo le patte **automatiche**; `reason`: 0 lista semimosse
esaurita (replay delle famose: risultato storico, non un matto), 1 matto,
2 stallo, 3 regola dei 75 halfmove, 4 ripetizione 5 volte, 5 materiale
insufficiente. Le patte reclamabili (50 halfmove, tripla ripetizione) NON
chiudono la partita: arrivano come flag `F_DRAW50`/`F_DRAWREP` e la UI le
mostra come avviso.
`winner`: 0 bianco, 1 nero, `0xFF` = patta.

## Replay delle partite famose

Le 30 partite sono archiviate in flash sull'ESP32 (`esp32/famous_games.h`,
generate e validate da `tools/pgn2moves.c`, una per volta:

1. il C64 manda `FAME_LOAD` + indice: l'ESP32 riparte dalla posizione
   iniziale e risponde `ACK` + n lo + n hi;
2. il C64 manda `FAME_NEXT` per ogni semimossa (con la sua cadenza di
   display): l'ESP32 la applica e risponde `MOVED` + flags + mossa lo/hi,
   così il C64 disegna solo le case toccate e calcola la SAN locale;
3. dopo l'ultima semimossa un ulteriore `FAME_NEXT` chiude con
   `GAME_OVER` reason 0 e il risultato storico.

## Flusso di una partita

1. Il C64 manda `SET_COLOR`, `SET_LEVEL`, `NEW_GAME` (ognuno con `ACK`).
2. Se l'umano è **nero**, l'ESP32 risponde subito con `BEST_MOVE` (il computer
   bianco apre); altrimenti tocca all'umano.
3. Ad ogni `MOVE` dell'umano l'ESP32 risponde:
   - `ILLEGAL` → l'umano ripete;
   - `MOVED` (scacco dato e/o patta reclamabile) subito dopo l'accettazione,
     poi `GAME_OVER` (l'umano ha chiuso la partita: matto/stallo/patta) o
     `BEST_MOVE` → mossa del computer (ed eventuale `GAME_OVER` in coda).

## Resync dopo un errore di collegamento

Ogni byte C64→ESP32 è confermato dall'ESP32 (abbassa D7 quando l'ISR ha
catturato il byte): `comm_tx_byte` attende l'ack e ritorna errore se non
arriva. Alla prima anomalia (tx fallito, risposta ignota, timeout) la UI:

1. svuota il collegamento (`drain_link`: consuma i byte in volo finché non
   arriva silenzio);
2. reinvia `SET_COLOR` e `SET_LEVEL` (copre anche un reboot dell'ESP32);
3. manda `GET_HIST` e ricostruisce board locale e storico dalla risposta
   `HIST` (lo stato vero è sempre quello dell'ESP32);
4. se dopo il resync tocca al computer, manda `GO` e ne consuma la risposta.

La partita così non si perde: dopo un errore si continua dalla posizione
vera invece di ricominciare o di uscire in silenzio a BASIC.
