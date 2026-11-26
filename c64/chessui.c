/*
 * chessui.c - UI C64 "stupida" per esp32chess (bozza).
 *
 * Il C64 NON genera mosse ne' valuta: mantiene solo una copia della posizione
 * per il display e per il cursore, manda le mosse all'ESP32 e ne riceve la
 * risposta. La legalita' e' decisa dall'ESP32.
 *
 * Bozza: board testuale con conio. Da sostituire con la UI grafica 3x3-charset
 * di CHESS 2026 (~/lavoro/chess2026/src/main_c64.c).
 */

#include <conio.h>
#include <string.h>
#include "protocol.h"
#include "comm.h"

/* ---- tipi pezzo (condivisi con il motore) ---- */
#define EMPTY  0
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6
#define WHITE  0
#define BLACK  1

#define PT(p) ((unsigned char)((p) & 7))
#define PC(p) ((unsigned char)((p) >> 3))
#define MAKE_PIECE(t,c) ((unsigned char)((t) | ((c) << 3)))

/* ---- codifica mossa (identica al motore) ---- */
#define MK_MOVE(f,t,pr) ((unsigned int)((f) | ((t) << 7) | ((pr) << 14)))
#define M_FROM(m) ((unsigned char)((m) & 0x7F))
#define M_TO(m)   ((unsigned char)(((m) >> 7) & 0x7F))
#define M_PROMO(m) ((unsigned char)(((m) >> 14) & 3))
#define MOVE_NONE ((unsigned int)0xFFFF)

static const unsigned char promoType[4] = { KNIGHT, BISHOP, ROOK, QUEEN };

/* ---- stato locale (solo display) ---- */
static unsigned char lboard[128];
static unsigned char lside;
static signed char   lep;               /* casa en passant (per il display) */

/* ---- storico locale per UNDO ---- */
static unsigned int  hist[400];
static unsigned int  histN;

static unsigned char humanColor;
static unsigned char level;

/* ================================================================== */
/* board locale                                                        */
/* ================================================================== */

static void new_board_local(void) {
  unsigned char i;
  memset(lboard, 0, sizeof(lboard));
  lboard[0]  = MAKE_PIECE(ROOK, WHITE);
  lboard[1]  = MAKE_PIECE(KNIGHT, WHITE);
  lboard[2]  = MAKE_PIECE(BISHOP, WHITE);
  lboard[3]  = MAKE_PIECE(QUEEN, WHITE);
  lboard[4]  = MAKE_PIECE(KING, WHITE);
  lboard[5]  = MAKE_PIECE(BISHOP, WHITE);
  lboard[6]  = MAKE_PIECE(KNIGHT, WHITE);
  lboard[7]  = MAKE_PIECE(ROOK, WHITE);
  lboard[112] = MAKE_PIECE(ROOK, BLACK);
  lboard[113] = MAKE_PIECE(KNIGHT, BLACK);
  lboard[114] = MAKE_PIECE(BISHOP, BLACK);
  lboard[115] = MAKE_PIECE(QUEEN, BLACK);
  lboard[116] = MAKE_PIECE(KING, BLACK);
  lboard[117] = MAKE_PIECE(BISHOP, BLACK);
  lboard[118] = MAKE_PIECE(KNIGHT, BLACK);
  lboard[119] = MAKE_PIECE(ROOK, BLACK);
  for (i = 0; i < 8; i++) {
    lboard[16 + i]  = MAKE_PIECE(PAWN, WHITE);
    lboard[96 + i]  = MAKE_PIECE(PAWN, BLACK);
  }
  lside = WHITE;
  lep = -1;
}

/* Applica una mossa alla copia locale (solo per il display: gestisce cattura,
 * promozione, arrocco ed en passant ma non verifica la legalita'). */
static void apply_move_local(unsigned int m) {
  unsigned char from  = M_FROM(m);
  unsigned char to    = M_TO(m);
  unsigned char promo = M_PROMO(m);
  unsigned char p     = lboard[from];
  unsigned char pt    = PT(p);
  unsigned char pc    = PC(p);

  lboard[to]   = p;
  lboard[from] = EMPTY;

  /* promozione */
  if (pt == PAWN && (to < 16 || to >= 112))
    lboard[to] = MAKE_PIECE(promoType[promo], pc);

  /* en passant: rimuove il pedone catturato dietro la casa d'arrivo */
  if (pt == PAWN && to == (unsigned char)lep)
    lboard[(unsigned char)(to + (pc == WHITE ? -16 : 16))] = EMPTY;
  lep = -1;

  /* doppia spinta di pedone: registra la casa en passant */
  if (pt == PAWN && (from - to == 32 || to - from == 32))
    lep = (signed char)((from + to) >> 1);

  /* arrocco: sposta la torre */
  if (pt == KING && (to - from == 2 || from - to == 2)) {
    if (to > from) {                              /* lato re */
      lboard[(unsigned char)(to - 1)] = lboard[(unsigned char)(to + 1)];
      lboard[(unsigned char)(to + 1)] = EMPTY;
    } else {                                      /* lato donna */
      lboard[(unsigned char)(to + 1)] = lboard[(unsigned char)(to - 2)];
      lboard[(unsigned char)(to - 2)] = EMPTY;
    }
  }

  lside ^= 1;
}

/* ================================================================== */
/* disegno                                                             */
/* ================================================================== */

static void draw_board(void) {
  static const char glyph[7] = " PNBRQK";
  unsigned char r, f, sq, p;

  clrscr();
  textcolor(COLOR_CYAN);
  cprintf("esp32chess (draft)\r\n\r\n");

  textcolor(COLOR_WHITE);
  cprintf("  a b c d e f g h\r\n");
  for (r = 8; r >= 1; r--) {
    textcolor(COLOR_WHITE);
    cprintf("%d ", r);
    for (f = 0; f < 8; f++) {
      sq = (unsigned char)(((r - 1) << 4) | f);
      p  = lboard[sq];
      if (p) {
        textcolor(PC(p) ? COLOR_YELLOW : COLOR_WHITE);
        cputc(glyph[PT(p)]);
      } else {
        textcolor(COLOR_GRAY2);
        cputc('.');
      }
      cputc(' ');
    }
    cprintf("\r\n");
  }
  textcolor(COLOR_WHITE);
  cprintf("  a b c d e f g h\r\n");

  textcolor(COLOR_GREEN);
  cprintf("You: %s   Level: %d\r\n",
          humanColor ? "black" : "white", (int)level + 1);
}

/* e2e4 -> "e2e4" */
static void move_str(unsigned int m, char *out) {
  out[0] = (char)('a' + (M_FROM(m) & 7));
  out[1] = (char)('1' + (M_FROM(m) >> 4));
  out[2] = (char)('a' + (M_TO(m) & 7));
  out[3] = (char)('1' + (M_TO(m) >> 4));
  out[4] = 0;
}

/* ================================================================== */
/* input                                                               */
/* ================================================================== */

static void read_line(char *buf, unsigned char max) {
  unsigned char n = 0, c;
  for (;;) {
    c = cgetc();
    if (c == 0x0D) {                     /* RETURN */
      cputc('\r');
      cputc('\n');
      break;
    }
    if (c == 0x14 || c == 0x08 || c == 0x7F) {   /* DEL / backspace */
      if (n) {
        n--;
        gotoxy(wherex() - 1, wherey());
        cputc(' ');
        gotoxy(wherex() - 1, wherey());
      }
      continue;
    }
    if (n < max && c >= 0x20 && c < 0x7F) {
      buf[n++] = (char)c;
      cputc(c);
    }
  }
  buf[n] = 0;
}

/* "e2e4", "e2-e4", "e2 e4", "e7e8q" -> mossa 16 bit (o MOVE_NONE).
 * La promozione di default e' la donna (solo per i pedoni che promuovono). */
static unsigned int parse_local(const char *s) {
  unsigned char f0, r0, f1, r1, promo = 3, isPromo;
  unsigned char p;

  while (*s == ' ') s++;
  if (*s >= 'a' && *s <= 'h')      f0 = (unsigned char)(*s - 'a');
  else if (*s >= 'A' && *s <= 'H') f0 = (unsigned char)(*s - 'A');
  else return MOVE_NONE;
  s++;
  if (*s < '1' || *s > '8') return MOVE_NONE;
  r0 = (unsigned char)(*s - '1');
  s++;
  if (*s == '-' || *s == ' ') s++;
  if (*s >= 'a' && *s <= 'h')      f1 = (unsigned char)(*s - 'a');
  else if (*s >= 'A' && *s <= 'H') f1 = (unsigned char)(*s - 'A');
  else return MOVE_NONE;
  s++;
  if (*s < '1' || *s > '8') return MOVE_NONE;
  r1 = (unsigned char)(*s - '1');
  s++;
  if (*s == '-' || *s == ' ') s++;
  if (*s) {
    if (*s == 'q' || *s == 'Q')      promo = 3;
    else if (*s == 'n' || *s == 'N') promo = 0;
    else if (*s == 'b' || *s == 'B') promo = 1;
    else if (*s == 'r' || *s == 'R') promo = 2;
    else return MOVE_NONE;
  }

  p = lboard[(unsigned char)((r0 << 4) | f0)];
  isPromo = (PT(p) == PAWN && (r1 == 0 || r1 == 7));
  if (!isPromo) promo = 0;

  return MK_MOVE((unsigned char)((r0 << 4) | f0),
                 (unsigned char)((r1 << 4) | f1),
                 promo);
}

/* ================================================================== */
/* protocollo                                                          */
/* ================================================================== */

static void wait_ack(void) {
  (void)comm_rx_byte();                 /* attende (e ignora) la risposta */
}

static void send_move(unsigned int m) {
  comm_tx_byte(CMD_MOVE);
  comm_tx_byte((unsigned char)(m & 0xFF));
  comm_tx_byte((unsigned char)((m >> 8) & 0xFF));
}

static void show_game_over(unsigned char reason, unsigned char winner) {
  static const char *msg[5] = {
    "CHECKMATE", "STALEMATE", "50 MOVE DRAW", "REPETITION", "MATERIAL"
  };
  textcolor(COLOR_LIGHTRED);
  if (reason >= 1 && reason <= 5)
    cprintf("\r\n%s!\r\n", msg[reason - 1]);
  if (reason == OVER_MATE)
    cprintf(winner == WINNER_WHITE ? "WHITE WINS" : "BLACK WINS");
  else
    cprintf("DRAW");
  cprintf("\r\npress a key...\r\n");
  (void)cgetc();
}

/* Legge il payload di BEST_MOVE (il byte RSP_BEST_MOVE e' gia' stato letto),
 * applica la mossa del computer e mostra punteggio/profondita'.
 * Ritorna 1 se la partita e' finita. */
static unsigned char recv_best_payload(void) {
  char buf[6];
  unsigned int  m = (unsigned int)comm_rx_byte() | ((unsigned int)comm_rx_byte() << 8);
  int score      = (int)((unsigned int)comm_rx_byte() | ((unsigned int)comm_rx_byte() << 8));
  unsigned char depth = comm_rx_byte();
  unsigned char flags = comm_rx_byte();

  apply_move_local(m);
  if (histN < 400) hist[histN++] = m;

  move_str(m, buf);
  textcolor(COLOR_CYAN);
  cprintf("AI: %s (val %d, d%d)\r\n", buf, score, (int)depth);

  if (flags & F_CHECK) {
    textcolor(COLOR_LIGHTRED);
    cprintf("CHECK!\r\n");
  }
  if (flags & F_GAMEOVER) {
    unsigned char reason = comm_rx_byte();
    unsigned char winner = comm_rx_byte();
    show_game_over(reason, winner);
    return 1;
  }
  return 0;
}

static void do_undo(void) {
  unsigned int i;
  if (histN < 2) {
    textcolor(COLOR_LIGHTRED);
    cprintf("nothing to undo\r\n");
    return;
  }
  comm_tx_byte(CMD_UNDO);
  wait_ack();

  histN -= 2;
  new_board_local();
  for (i = 0; i < histN; i++) apply_move_local(hist[i]);
}

/* ================================================================== */
/* loop di gioco                                                       */
/* ================================================================== */

static void game_loop(void) {
  char buf[12];
  for (;;) {
    draw_board();
    cprintf("\r\nmove: ");
    read_line(buf, 8);

    if (buf[0] == '.') {                  /* esci */
      comm_tx_byte(CMD_QUIT);
      wait_ack();
      return;
    }
    if (buf[0] == 'u' || buf[0] == 'U') { /* undo */
      do_undo();
      continue;
    }
    {
      unsigned int m = parse_local(buf);
      unsigned char from, p, rsp;

      if (m == MOVE_NONE) {
        textcolor(COLOR_LIGHTRED);
        cprintf("bad move\r\n");
        continue;
      }

      /* controllo locale "leggero" (l'ESP32 decide comunque la legalita') */
      from = M_FROM(m);
      p    = lboard[from];
      if (p == EMPTY || PC(p) != lside) {
        textcolor(COLOR_LIGHTRED);
        cprintf("not yours\r\n");
        continue;
      }

      send_move(m);
      rsp = comm_rx_byte();

      if (rsp == RSP_ILLEGAL) {
        textcolor(COLOR_LIGHTRED);
        cprintf("bad move\r\n");
        continue;
      }
      if (rsp == RSP_GAME_OVER) {         /* l'umano ha chiuso la partita */
        unsigned char reason = comm_rx_byte();
        unsigned char winner = comm_rx_byte();
        show_game_over(reason, winner);
        return;
      }
      if (rsp == RSP_BEST_MOVE) {         /* partita che prosegue */
        apply_move_local(m);
        if (histN < 400) hist[histN++] = m;
        if (recv_best_payload()) return;
      }
    }
  }
}

/* ================================================================== */

int main(void) {
  char c;

  clrscr();
  textcolor(COLOR_WHITE);
  bgcolor(COLOR_BLUE);
  bordercolor(COLOR_BLUE);

  comm_install();
  new_board_local();
  humanColor = WHITE;
  level      = LEVEL_MEDIUM;

  /* menu */
  cprintf("Your color? (w/b) ");
  do { c = cgetc(); } while (c != 'w' && c != 'W' && c != 'b' && c != 'B');
  humanColor = (c == 'b' || c == 'B') ? BLACK : WHITE;
  cputc(c);
  cprintf("\r\n");

  cprintf("Level? (1-4) ");
  do { c = cgetc(); } while (c < '1' || c > '4');
  level = (unsigned char)(c - '1');
  cputc(c);
  cprintf("\r\n");

  comm_tx_byte(CMD_SET_COLOR); comm_tx_byte(humanColor); wait_ack();
  comm_tx_byte(CMD_SET_LEVEL); comm_tx_byte(level);      wait_ack();
  comm_tx_byte(CMD_NEW_GAME);  wait_ack();

  if (humanColor == BLACK) {
    /* il computer (bianco) apre: BEST_MOVE (o GAME_OVER) subito dopo l'ACK */
    unsigned char rsp = comm_rx_byte();
    if (rsp == RSP_BEST_MOVE) {
      if (recv_best_payload()) return 0;
    } else if (rsp == RSP_GAME_OVER) {
      unsigned char reason = comm_rx_byte();
      unsigned char winner = comm_rx_byte();
      show_game_over(reason, winner);
      return 0;
    }
  }

  game_loop();
  return 0;
}
