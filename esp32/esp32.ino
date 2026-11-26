/*
 * esp32.ino - motore scacchi su ESP32 per la cartuccia c64esp32cart.
 *
 * L'ESP32 e' il SOLO arbitro: tiene lo stato vero della partita (board,
 * zobrist, storico, valutazione) e decide la legalita' delle mosse.
 * Il C64 fa solo da UI e dialoga con questo firmware via protocollo (vedi
 * docs/PROTOCOL.md e esp43chess/AGENTS.md §4).
 *
 * Trasporto predefinito: Serial 115200 (per test da terminale/Python).
 * Con -DUSE_C64_BUS si usa il bus della cartuccia ($DE00/$DF00 + NMI).
 */

#include <Arduino.h>

extern "C" {
#include "chess.h"
}

#include "protocol.h"

/* Debug su seriale: SOLO in modalita' BUS (in seriale la Serial e' il
 * trasporto del protocollo, non va sporcata con testo). */
#if defined(USE_C64_BUS)
#define DBG(...) Serial.printf(__VA_ARGS__)
#else
#define DBG(...) do{}while(0)
#endif

/* ================================================================== */
/* Trasporto verso il C64                                              */
/* ================================================================== */

#if defined(USE_C64_BUS)

/* Pin della cartuccia (da c64esp32cart/c64esp32Cart.ino) */
static const uint8_t busD[8] = { 4, 33, 14, 27, 13, 22, 17, 26 }; /* D0..D7 */
#define oC64NMI 32
#define sclk    25
#define pload   16
#define sdata   34
#define C64IO1  35
#define C64IO2  36
#define oC64RST 21   /* /RESET del C64 (via 74LS14, non invertente: HIGH = run) */
#define CLED    2    /* /EXROM: LOW = EXROM alto = EPROM $8000 non mappata */
#define resetSwitch 15

static volatile bool dataFromC64 = false;
static volatile byte ch = 0;
static volatile bool io2 = false;
static volatile bool nmiArm = false;   /* abilita l'ack solo dopo l'annuncio /NMI */

/* Il C64 scrive su $DE00 (I/O1): abbassa D7 (flow control) e cattura il byte
 * dal 74HC597 (shift register). */
static void IRAM_ATTR isr_io1() {
  digitalWrite(busD[7], LOW);          /* non pronto per il prossimo byte */
  ch = 0;
  digitalWrite(pload, HIGH);           /* stop load, enable shift */
  ch = shiftIn(sdata, sclk, MSBFIRST);
  dataFromC64 = true;
  digitalWrite(pload, LOW);
}

/* Il C64 legge $DF00 (I/O2): segnala che il byte inviato e' stato letto.
 * Vale solo quando l'ESP32 ha annunciato un byte con /NMI: le letture di
 * polling di $DF00 nel ciclo di tx del C64 generano lo stesso fronte su
 * /I/O2 e non devono fare da ack (H3 in docs/BUGS.md). */
static void IRAM_ATTR isr_io2() {
  if (nmiArm) io2 = true;
}

static void outByte(byte c) {
  for (int i = 0; i < 8; i++) gpio_set_level((gpio_num_t)busD[i], (c >> i) & 1);
}

/* ESP32 -> C64: byte su D0..D7 + impulso su /NMI, poi attesa della lettura. */
static void busSendByte(byte b) {
  outByte(b);
  io2 = false;
  nmiArm = true;          /* solo le letture successive all'annuncio contano */
  digitalWrite(oC64NMI, HIGH);
  delayMicroseconds(150);
  digitalWrite(oC64NMI, LOW);
  uint32_t t0 = micros();
  while (!io2) {
    if (micros() - t0 > 50000) break;  /* timeout 50 ms: abbandona */
    delayMicroseconds(10);
  }
  nmiArm = false;
  delayMicroseconds(100); /* margine: l'handler NMI completa la copia */
  io2 = false;
}

/* C64 -> ESP32: alza D7 (pronto) e attende che l'ISR catturi un byte.
 * Ritorna il byte, oppure -1 su timeout. */
static int busReadByte(uint32_t timeoutMs) {
  gpio_set_level((gpio_num_t)busD[7], HIGH);   /* pronto a ricevere */
  uint32_t t0 = millis();
  while (!dataFromC64) {
    if (timeoutMs && millis() - t0 > timeoutMs) {
      gpio_set_level((gpio_num_t)busD[7], LOW);
      return -1;
    }
    delayMicroseconds(2);
  }
  gpio_set_level((gpio_num_t)busD[7], LOW);
  dataFromC64 = false;
  return (int)ch;
}

static void linkSetup() {
  Serial.begin(115200);                 /* solo debug in modalita' BUS */
  for (int i = 0; i < 8; i++) { pinMode(busD[i], OUTPUT); gpio_set_level((gpio_num_t)busD[i], 0); }
  pinMode(oC64NMI, OUTPUT); digitalWrite(oC64NMI, LOW);
  pinMode(sclk, OUTPUT);     digitalWrite(sclk, LOW);
  pinMode(pload, OUTPUT);    digitalWrite(pload, LOW);
  pinMode(sdata, INPUT);
  pinMode(C64IO1, INPUT);
  pinMode(C64IO2, INPUT);
  attachInterrupt(C64IO1, isr_io1, RISING);
  attachInterrupt(C64IO2, isr_io2, FALLING);

  /* Boot del C64: senza questi due pin il C64 resta in reset (schermo nero).
   * EXROM alto = EPROM $8000 NON mappata -> il C64 boota in BASIC normale;
   * /RESET alto = C64 in run. Poi un impulso di reset per un boot pulito
   * anche se il C64 e' stato acceso mentre l'ESP32 stava ancora partendo. */
  pinMode(CLED, OUTPUT);     digitalWrite(CLED, LOW);      /* EXROM alto */
  pinMode(oC64RST, OUTPUT);  digitalWrite(oC64RST, HIGH);  /* run */
  pinMode(resetSwitch, INPUT_PULLUP);
  delay(50);
  digitalWrite(oC64RST, LOW);                              /* reset */
  delay(250);
  digitalWrite(oC64RST, HIGH);                             /* run */

  gpio_set_level((gpio_num_t)busD[7], HIGH);   /* pronto */
  Serial.println("[chess] linkSetup ok");
}
static int  linkReadByte(uint32_t timeoutMs) { return busReadByte(timeoutMs); }
static void linkWriteByte(byte b)            { busSendByte(b); }

#else

/* Trasporto seriale: comodo per la prima verifica del motore. */
static void linkSetup() { Serial.begin(115200); }
static int linkReadByte(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (!Serial.available()) {
    if (timeoutMs && millis() - t0 > timeoutMs) return -1;
    delay(1);
  }
  return Serial.read();
}
static void linkWriteByte(byte b) { Serial.write(b); }

#endif /* USE_C64_BUS */

/* ================================================================== */
/* Sessione di gioco                                                   */
/* ================================================================== */

static uint8_t humanColor = 0;      /* WHITE */
static uint8_t level      = 1;      /* LEVEL_MEDIUM */

static u16 moveList[MAXMOVES];      /* buffer per legal_moves() */
static u16 gameHist[400];           /* mosse giocate (per UNDO) */
static u16 gameHistN = 0;

static uint8_t levelDepth(uint8_t lv) {
  switch (lv) {
    case LEVEL_EASY:   return 2;
    case LEVEL_MEDIUM: return 4;
    case LEVEL_HARD:   return 6;
    default:           return 8;    /* LEVEL_EXPERT */
  }
}

static uint8_t levelSeconds(uint8_t lv) {
  switch (lv) {
    case LEVEL_EASY:   return 0;
    case LEVEL_MEDIUM: return 1;
    case LEVEL_HARD:   return 3;
    default:           return 6;
  }
}

static uint8_t gameOverReason(void) {
  if (!has_legal_moves())
    return in_check() ? OVER_MATE : OVER_STALEMATE;
  if (fifty_moves())          return OVER_50MOVE;
  if (repetition_count() >= 2) return OVER_REPETITION;
  if (insufficient_material()) return OVER_MATERIAL;
  return 0;
}

static uint8_t winnerOf(uint8_t reason) {
  return (reason == OVER_MATE) ? (uint8_t)(side ^ 1) : WINNER_DRAW;
}

/* Cerca la mossa umana tra quelle legali (l'ESP32 decide la legalita'). */
static u16 engineFindLegalMove(u16 m) {
  u8 n = legal_moves(moveList);
  for (u8 i = 0; i < n; i++)
    if (moveList[i] == m) return m;
  return MOVE_NONE;
}

static void recordMove(u16 m) {
  if (gameHistN < 400) gameHist[gameHistN++] = m;
}

static void sendGameOver(uint8_t over) {
  linkWriteByte(RSP_GAME_OVER);
  linkWriteByte(over);
  linkWriteByte(winnerOf(over));
}

/* Storico completo: serve alla UI per ricostruire la board locale dopo un
 * errore di collegamento (resync) senza buttare la partita. */
static void sendHist(void) {
  linkWriteByte(RSP_HIST);
  linkWriteByte((uint8_t)(gameHistN & 0xFF));
  linkWriteByte((uint8_t)((gameHistN >> 8) & 0xFF));
  for (u16 i = 0; i < gameHistN; i++) {
    linkWriteByte((uint8_t)(gameHist[i] & 0xFF));
    linkWriteByte((uint8_t)((gameHist[i] >> 8) & 0xFF));
  }
}

/* Pensa, applica e invia la mossa del computer (BEST_MOVE + eventuale
 * GAME_OVER in coda). */
static void sendBestMove(void) {
  s16 score;
  quiesceMax   = 4;
  thinkSeconds = levelSeconds(level);
  DBG("BEST: think(depth=%d, sec=%d)...\n", levelDepth(level), thinkSeconds);

  u16 best = think(levelDepth(level), 0, &score);
  DBG("BEST: think done, best=0x%04X score=%d\n", (unsigned)best, (int)score);
  if (best == MOVE_NONE) {           /* non dovrebbe accadere qui */
    sendGameOver(gameOverReason());
    return;
  }

  commit_move(best);
  recordMove(best);

  uint8_t flags = 0;
  if (in_check()) flags |= F_CHECK;  /* l'umano e' sotto scacco */
  uint8_t over = gameOverReason();
  if (over) flags |= F_GAMEOVER;

  linkWriteByte(RSP_BEST_MOVE);
  linkWriteByte((uint8_t)(best & 0xFF));
  linkWriteByte((uint8_t)((best >> 8) & 0xFF));
  linkWriteByte((uint8_t)((u16)score & 0xFF));
  linkWriteByte((uint8_t)(((u16)score >> 8) & 0xFF));
  linkWriteByte(thinkDepth);
  linkWriteByte(flags);

  if (over) sendGameOver(over);
}

/* ---- gestori dei comandi ---- */

static void handleNewGame(void) {
  new_game();
  gameHistN = 0;
  linkWriteByte(RSP_ACK);
  if (humanColor == BLACK) sendBestMove();   /* il computer (bianco) apre */
}

static void handleSetColor(void) {
  int c = linkReadByte(2000);
  if (c >= 0) humanColor = (uint8_t)(c & 1);
  linkWriteByte(RSP_ACK);
}

static void handleSetLevel(void) {
  int l = linkReadByte(2000);
  if (l >= 0) level = (uint8_t)(l & 3);
  linkWriteByte(RSP_ACK);
}

static void handleMove(void) {
  DBG("MOVE: leggo lo/hi\n");
  int lo = linkReadByte(2000);
  int hi = linkReadByte(2000);
  DBG("MOVE: lo=%d hi=%d\n", lo, hi);
  if (lo < 0 || hi < 0) {
    /* parametro perso: senza risposta il C64 attenderebbe per sempre */
    DBG("MOVE: timeout sui byte\n");
    linkWriteByte(RSP_ILLEGAL);
    return;
  }
  u16 m = (u16)(lo | (hi << 8));
  DBG("MOVE: m=0x%04X\n", (unsigned)m);

  u16 legal = engineFindLegalMove(m);
  if (legal == MOVE_NONE) {
    DBG("MOVE: ILLEGAL\n");
    linkWriteByte(RSP_ILLEGAL);
    return;
  }

  commit_move(legal);
  recordMove(legal);

  /* conferma immediata della mossa con l'eventuale scacco dato: arriva
   * PRIMA del think, cosi' la UI puo' segnalarlo senza aspettare la
   * risposta del computer (RSP_MOVED e' retrocompatibile: la vecchia UI
   * lo vede come risposta ignota ma la nuova lo gestisce) */
  uint8_t givesCheck = in_check_of((uint8_t)(side ^ 1));
  linkWriteByte(RSP_MOVED);
  linkWriteByte(givesCheck ? F_CHECK : 0);

  uint8_t over = gameOverReason();
  if (over) { sendGameOver(over); return; }

  DBG("MOVE: legale, chiamo think()\n");
  sendBestMove();
  DBG("MOVE: risposta inviata\n");
}

/* UNDO: ricostruisce la partita dall'inizio riapplicando tutte le mosse
 * tranne le ultime due. Semplice e corretto (niente undo incrementale). */
static void handleUndo(void) {
  if (gameHistN >= 2) {
    gameHistN -= 2;
    new_game();
    for (u16 i = 0; i < gameHistN; i++) commit_move(gameHist[i]);
  }
  linkWriteByte(RSP_ACK);
}

static void handleQuit(void) {
  new_game();
  gameHistN = 0;
  linkWriteByte(RSP_ACK);
}

static void handleGetHist(void) {
  sendHist();
}

/* Se e' il turno del computer muove (dopo un resync o all'avvio da UI);
 * altrimenti conferma soltanto. */
static void handleGo(void) {
  if ((uint8_t)side == humanColor) {
    linkWriteByte(RSP_ACK);
    return;
  }
  sendBestMove();
}

static void dispatch(uint8_t cmd) {
  DBG("[chess] CMD %d (0x%02X)\n", (int)cmd, (int)cmd);
  switch (cmd) {
    case CMD_NEW_GAME:  handleNewGame();  break;
    case CMD_SET_COLOR: handleSetColor(); break;
    case CMD_SET_LEVEL: handleSetLevel(); break;
    case CMD_MOVE:      handleMove();     break;
    case CMD_UNDO:      handleUndo();     break;
    case CMD_QUIT:      handleQuit();     break;
    case CMD_GET_HIST:  handleGetHist();  break;
    case CMD_GO:        handleGo();       break;
    default: break;                       /* comando ignoto: ignora */
  }
}

/* ================================================================== */

void setup() {
  linkSetup();
  init_engine();
  new_game();
  gameHistN = 0;
  DBG("[chess] setup done, in loop\n");
}

void loop() {
  int cmd = linkReadByte(0);   /* attende un comando (0 = nessun timeout) */
  if (cmd >= 0) dispatch((uint8_t)cmd);
}
