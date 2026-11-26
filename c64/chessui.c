/*
 * chessui.c - UI C64 per esp32chess: scacchiera grafica 3x3-charset
 * (charset custom in banca VIC 2), effetti sonori SID, joystick e due
 * timer (giocatore e AI). La mossa del giocatore viene mostrata subito
 * sulla scacchiera, prima ancora che l'ESP32 risponda.
 *
 * Il C64 NON genera mosse ne' valuta: mantiene solo una copia della
 * posizione per il display (lboard); la legalita' la decide sempre l'ESP32.
 *
 * Schermo a $8400, charset a $8800 (banca VIC 2): il BSS deve restare
 * sotto $8400 (controllare la map del linker dopo modifiche grosse).
 */

#include <conio.h>
#include <c64.h>
#include <peekpoke.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "protocol.h"
#include "comm.h"
#include "font_writer.h"

/* ---- tipi (cc65: int = 16 bit) ---- */
typedef unsigned char  u8;
typedef signed char    s8;
typedef unsigned int   u16;
typedef signed int     s16;
typedef unsigned long  u32;
typedef signed long    s32;

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

/* timeout di ricezione (ms): la risposta a un comando puo' arrivare dopo il
 * think del motore (fino a 6s su EXPERT), i byte successivi arrivano a
 * raffica. Su timeout comm_rx_byte svuota il buffer e ritorna -1. */
#define RX_TIMEOUT_CMD   10000u
#define RX_TIMEOUT_BYTE   2000u

/* jiffy (1/60 s) per millisecondi, approssimato */
#define MS_TO_JFY(ms) ((u32)(ms) / 17UL)

/* ---- codifica mossa (identica al motore) ---- */
#define MK_MOVE(f,t,pr) ((u16)((f) | ((t) << 7) | ((pr) << 14)))
#define M_FROM(m) ((u8)((m) & 0x7F))
#define M_TO(m)   ((u8)(((m) >> 7) & 0x7F))
#define M_PROMO(m) ((u8)(((m) >> 14) & 3))
#define MOVE_NONE ((u16)0xFFFF)
#define MOVE_UNDO ((u16)0xFFFE)     /* richiede from 0x7E/to 0x7F: irraggiungibile */

static const unsigned char promoType[4] = { KNIGHT, BISHOP, ROOK, QUEEN };

/* ---- layout: schermo in banca VIC 2, scacchiera 3x3 caratteri per casa ---- */
#define SCR_BASE   0x8400
#define COL_BASE   0xD800
#define CHR_BASE   0x8800

#define BOARD_X    14
#define BOARD_Y    1
#define FILE_ROW   0
#define RANK_COL_R 38

#define MOVE_ROW   2
#define VAL_ROW    5
#define DEPTH_ROW  6
#define CHECK_ROW  8
#define TIME_YOU_ROW 10
#define TIME_AI_ROW  11
#define PROMPT_ROW 20
#define MSG_ROW    21
#define OVER_ROW   22
#define OVER_WHO_ROW 23
#define OVER_KEY_ROW 24
#define PANEL_W    13

/* ---- stato locale (solo display) ---- */
static u8 lboard[128];
static u8 lside;
static s8 lep;                      /* casa en passant (per il display) */

static u16 hist[400];
static u16 histN;

static u8 humanColor;
static u8 level;
static u8 flip;                     /* 1 = scacchiera ruotata (umano nero) */

/* ultima mossa (0xFF = nessuna) e cursore joystick */
static u8 h1, h2;
static u8 joyCursor;                /* casa 0x88 del cursore */
static u8 joyFrom;                  /* casa selezionata (0xFF = nessuna) */

static char aiMove[8];              /* ultima mossa del computer (e2-e4) */
static u8 hasAiMove;                /* 1 = c'e' una mossa del computer da mostrare */
static s16 aiScore;
static u8 aiDepth;
static u8 checkFlag;                /* 0 = nessuno, 1 = sei sotto scacco,
                                       2 = la tua mossa ha dato scacco */
static u8 emptyBoard;               /* 1 = scacchiera senza pezzi (menu) */
static u8 g_wantNew;                /* scelta dell'utente a fine partita */

/* timer (jiffy): segmento aperto = l'orologio corre per quel lato */
#define SEG_PLAYER 0
#define SEG_AI     1
static u32 plBase, aiBase;          /* jiffy accumulati nei segmenti chiusi */
static u32 segT0;                   /* clock() di apertura del segmento */
static u8  segWho;                  /* SEG_PLAYER o SEG_AI */

/* snapshot per la mossa del giocatore: visibile subito, ripristinata
 * se l'ESP32 la dichiara illegale */
static u8 snapBoard[128];
static s8 snapLep;
static u8 snapSide, snapH1, snapH2;

/* ================================================================== */
/* glyph dei pezzi (3x3, 5 celle "a croce"; gli angoli usano lo spazio) */
/* ordine celle: top, left, center, right, bottom (8 byte ciascuna)    */
/* ================================================================== */

static const u8 glyph_solid[6][5][8] = {
    /* pawn */ {
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x1C, 0x3E, 0x67 },
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x5F, 0x5F, 0x7F, 0x3E, 0x1C, 0x1C, 0x1C, 0x1C },
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x1C, 0x3E, 0x3E, 0x7F, 0x55, 0x7F, 0x00, 0x00 },
    },
    /* knight */ {
        { 0x00, 0x00, 0x00, 0x18, 0x1C, 0x7A, 0xFD, 0xBD },
        { 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0xFE, 0xFE, 0x3F, 0x7F, 0xF7, 0xF7, 0x76, 0x26 },
        { 0x80, 0x80, 0x40, 0x40, 0x80, 0x80, 0x00, 0x00 },
        { 0x1E, 0x1C, 0x3C, 0x7E, 0xDB, 0xFF, 0x00, 0x00 },
    },
    /* bishop */ {
        { 0x00, 0x00, 0x00, 0x00, 0x3C, 0x5E, 0xBF, 0xBF },
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0xFF, 0x7E, 0x3C, 0x3C, 0x7E, 0xFF, 0x7E, 0x3C },
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x3C, 0x3C, 0x7E, 0xFF, 0xA5, 0xFF, 0x00, 0x00 },
    },
    /* rook */ {
        { 0x00, 0x00, 0x00, 0x00, 0xB6, 0xB6, 0x7F, 0x7F },
        { 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x7F, 0xAA, 0xBF, 0x5E, 0x5E, 0x5E, 0x5E, 0x5E },
        { 0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x5E, 0x5E, 0xDF, 0xAA, 0x7F, 0xFF, 0x00, 0x00 },
    },
    /* queen */ {
        { 0x00, 0x00, 0x00, 0x00, 0x55, 0x55, 0xFF, 0xAB },
        { 0x03, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00 },
        { 0xBF, 0xDF, 0x5E, 0x5E, 0xDF, 0xBF, 0xDF, 0x5E },
        { 0xC0, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00 },
        { 0x5E, 0xDF, 0xBF, 0xBF, 0x81, 0xFF, 0x00, 0x00 },
    },
    /* king */ {
        { 0x00, 0x00, 0x00, 0x18, 0x18, 0x7E, 0x7E, 0x18 },
        { 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 },
        { 0x18, 0x3C, 0xFF, 0xDB, 0xE7, 0x7E, 0x3C, 0xFF },
        { 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00 },
        { 0x3C, 0x7E, 0x7E, 0xFF, 0xC3, 0xFF, 0x00, 0x00 },
    },
};

static const u8 glyph_outline[6][5][8] = {
    /* pawn */ {
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x1C, 0x22, 0x49 },
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x51, 0x41, 0x41, 0x22, 0x14, 0x14, 0x14, 0x14 },
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x22, 0x22, 0x41, 0x55, 0x41, 0x7F, 0x00, 0x00 },
    },
    /* knight */ {
        { 0x00, 0x00, 0x00, 0x18, 0x14, 0x62, 0x83, 0xA0 },
        { 0x03, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x00, 0xC8, 0x28, 0x48, 0x88, 0x99, 0x42, 0x32 },
        { 0x80, 0x80, 0x40, 0x40, 0x80, 0x80, 0x00, 0x00 },
        { 0x12, 0x12, 0x32, 0x42, 0x81, 0xFF, 0x00, 0x00 },
    },
    /* bishop */ {
        { 0x00, 0x00, 0x00, 0x00, 0x3C, 0x52, 0xA1, 0xA1 },
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0xC1, 0x42, 0x24, 0x24, 0x42, 0x81, 0x42, 0x24 },
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x24, 0x24, 0x42, 0xDB, 0x81, 0xFF, 0x00, 0x00 },
    },
    /* rook */ {
        { 0x00, 0x00, 0x00, 0x00, 0xB6, 0xB6, 0x49, 0x40 },
        { 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x55, 0xAA, 0xB5, 0x52, 0x52, 0x52, 0x52, 0x52 },
        { 0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x52, 0x52, 0xD5, 0xAA, 0x55, 0xFF, 0x00, 0x00 },
    },
    /* queen */ {
        { 0x00, 0x00, 0x00, 0x00, 0x55, 0xAA, 0x81, 0x81 },
        { 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00 },
        { 0x40, 0xA1, 0x52, 0x52, 0x91, 0x20, 0x91, 0x52 },
        { 0x80, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00 },
        { 0x52, 0x91, 0x91, 0xA1, 0x81, 0xFF, 0x00, 0x00 },
    },
    /* king */ {
        { 0x00, 0x00, 0x00, 0x18, 0x18, 0x66, 0x66, 0x18 },
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x18, 0x24, 0x42, 0x81, 0x42, 0x24, 0x24, 0xC3 },
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x24, 0x42, 0x42, 0x81, 0x81, 0xFF, 0x00, 0x00 },
    },
};

/* ================================================================== */
/* output testi (POKE diretto: conio scrive sempre a $0400)            */
/* ================================================================== */

static u8 to_screen(char c)
{
    if (c >= 'A' && c <= 'Z') return (u8)(c - 'A' + 1);
    if (c >= 'a' && c <= 'z') return (u8)(c - 'a' + 1);
    if (c >= '0' && c <= '9') return (u8)(c - '0' + 0x30);
    switch (c) {
        case ' ': return 0x20;
        case ':': return 0x3A;
        case '-': return 0x2D;
        case '.': return 0x2E;
        case ',': return 0x2C;
        case '/': return 0x2F;
        case '=': return 0x1F;
        case '!': return 0x21;
        case '(': return 0x28;
        case ')': return 0x29;
        case '?': return 0x3F;
        default:  return 0x20;
    }
}

static void scr_put(u8 x, u8 y, u8 code, u8 color)
{
    u16 o = (u16)(y * 40 + x);
    POKE((u16)(SCR_BASE + o), code);
    POKE((u16)(COL_BASE + o), color);
}

static void scr_puts(u8 x, u8 y, const char *s, u8 color)
{
    while (*s) {
        scr_put(x, y, to_screen(*s), color);
        x++;
        s++;
    }
}

static void scr_unum(u8 x, u8 y, unsigned int v, u8 color)
{
    char b[8];
    utoa(v, b, 10);
    scr_puts(x, y, b, color);
}

static void scr_snum(u8 x, u8 y, s16 v, u8 color)
{
    char b[8];
    itoa(v, b, 10);
    scr_puts(x, y, b, color);
}

static void scr_blank(u8 x, u8 y, u8 n, u8 color)
{
    while (n--) scr_put(x++, y, 0x20, color);
}

static void scr_clear(void)
{
    u16 i;
    for (i = 0; i < 1000; i++) {
        POKE((u16)(SCR_BASE + i), 0x20);
        POKE((u16)(COL_BASE + i), COLOR_WHITE);
    }
}

/* ------------------------------------------------------------------ */
/* effetti sonori SID (voce 1): note musicali con envelope, non       */
/* semplici tonfi: l'onda quadra a frequenza bassissima del primo     */
/* draft era in pratica una serie di click.                           */
/* ------------------------------------------------------------------ */

static void sid_wait(u8 j)
{
    u32 t = (u32)clock() + j;
    while ((u32)clock() < t) {}
}

/* nota singola: f = frequency word, wave = forma d'onda SENZA il bit di
   gate (0x01), jfy = jiffy di gate. Sustain pieno e release breve: suono
   pieno che sfuma invece di troncarsi di netto. */
static void sid_note(u16 f, u8 wave, u8 jfy)
{
    POKE(0xD418, 0x0F);            /* volume massimo */
    POKE(0xD405, 0x00);            /* attacco immediato */
    POKE(0xD406, 0xF3);            /* sustain 15, release breve */
    POKE(0xD402, 0x00);            /* pulse width 50% (solo onda impulso) */
    POKE(0xD403, 0x08);
    POKE(0xD400, (u8)(f & 0xFF));
    POKE(0xD401, (u8)(f >> 8));
    POKE(0xD404, (u8)(wave | 0x01));  /* gate on */
    sid_wait(jfy);
    POKE(0xD404, wave);            /* gate off */
}

/* "tock" di mossa: tick alto; sulle catture un tonfo piu' grave */
static void sfx_move(u8 capture)
{
    if (capture) {
        sid_note(0x218A, 0x10, 8);     /* triangle C5 */
        sid_note(0x0862, 0x10, 10);    /* triangle C3: tonfo */
    } else {
        sid_note(0x4313, 0x10, 6);     /* triangle C6: tick */
    }
}

/* errore: due tonfi gravi in onda quadra */
static void sfx_error(void)
{
    sid_note(0x070D, 0x40, 7);
    sid_wait(2);
    sid_note(0x070D, 0x40, 7);
}

/* scacco: doppio beep acuto */
static void sfx_check(void)
{
    sid_note(0x2A41, 0x40, 5);
    sid_wait(2);
    sid_note(0x2A41, 0x40, 5);
}

/* fine partita: tre note discendenti */
static void sfx_gameover(void)
{
    sid_note(0x218A, 0x10, 10);
    sid_note(0x1920, 0x10, 10);
    sid_note(0x10C5, 0x10, 14);
}

/* ================================================================== */
/* charset custom                                                     */
/* ================================================================== */

static void install_charset(void)
{
    u8 t, cell, i;
    u16 j;

    for (j = 0; j < 2048; j++)
        POKE((u16)(CHR_BASE + j), 0x00);

    /* font testo: solo i glyph 0x00-0x3F (A-Z, cifre, punteggiatura) */
    for (j = 0; j < 512; j++)
        POKE((u16)(CHR_BASE + j), font_writer[j]);

    /* blocco solido (reverse-space) al glyph 0xA0: case vuote */
    for (j = 0; j < 8; j++)
        POKE((u16)(CHR_BASE + 0xA0 * 8 + j), 0xFF);

    /* pezzi: solid (neri) da glyph 64, outline (bianchi) da glyph 94.
       Memorizzati INVERTITI (0 = pezzo, 1 = sfondo casa). */
    for (t = 0; t < 6; t++) {
        for (cell = 0; cell < 5; cell++) {
            u16 a = (u16)(CHR_BASE + (64 + t * 5 + cell) * 8);
            u16 b = (u16)(CHR_BASE + (94 + t * 5 + cell) * 8);
            for (i = 0; i < 8; i++) {
                POKE((u16)(a + i), (u8)(0xFF ^ glyph_solid[t][cell][i]));
                POKE((u16)(b + i), (u8)(0xFF ^ glyph_outline[t][cell][i]));
            }
        }
    }
}

/* ================================================================== */
/* board locale                                                        */
/* ================================================================== */

static void new_board_local(void) {
    u8 i;
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
        lboard[16 + i] = MAKE_PIECE(PAWN, WHITE);
        lboard[96 + i] = MAKE_PIECE(PAWN, BLACK);
    }
    lside = WHITE;
    lep = -1;
}

/* Applica una mossa alla copia locale (solo per il display: gestisce cattura,
 * promozione, arrocco ed en passant ma non verifica la legalita'). */
static void apply_move_local(u16 m) {
    u8 from  = M_FROM(m);
    u8 to    = M_TO(m);
    u8 promo = M_PROMO(m);
    u8 p     = lboard[from];
    u8 pt    = PT(p);
    u8 pc    = PC(p);

    lboard[to]   = p;
    lboard[from] = EMPTY;

    if (pt == PAWN && (to < 16 || to >= 112))
        lboard[to] = MAKE_PIECE(promoType[promo], pc);

    if (pt == PAWN && to == (u8)lep)
        lboard[(u8)(to + (pc == WHITE ? -16 : 16))] = EMPTY;
    lep = -1;

    if (pt == PAWN && (from - to == 32 || to - from == 32))
        lep = (s8)((from + to) >> 1);

    if (pt == KING && (to - from == 2 || from - to == 2)) {
        if (to > from) {
            lboard[(u8)(to - 1)] = lboard[(u8)(to + 1)];
            lboard[(u8)(to + 1)] = EMPTY;
        } else {
            lboard[(u8)(to + 1)] = lboard[(u8)(to - 2)];
            lboard[(u8)(to - 2)] = EMPTY;
        }
    }

    lside ^= 1;
}

static u8 find_king(u8 color)
{
    u8 r, f, sq, p;
    for (r = 0; r < 8; r++)
        for (f = 0; f < 8; f++) {
            sq = (u8)((r << 4) | f);
            p = lboard[sq];
            if (PT(p) == KING && PC(p) == color) return sq;
        }
    return 0xFF;
}

/* ================================================================== */
/* scacchiera                                                         */
/* ================================================================== */

/* codice schermo della cella (cr,cc) della casa 3x3; 0xA0 = blocco solido */
static u8 piece_code(u8 p, u8 cr, u8 cc)
{
    u8 t, base, cell;

    if (!p) return 0xA0;

    t = (u8)(PT(p) - 1);
    base = (PC(p) == BLACK) ? 64 : 94;

    if (cr == 0 && cc == 1) cell = 0;
    else if (cr == 1 && cc == 0) cell = 1;
    else if (cr == 1 && cc == 1) cell = 2;
    else if (cr == 1 && cc == 2) cell = 3;
    else if (cr == 2 && cc == 1) cell = 4;
    else return 0xA0;

    return (u8)(base + t * 5 + cell);
}

/* colore della casa: giallo = destinazione ultima mossa o selezione,
   cyan = cursore joystick, altrimenti chiaro/scuro a scacchiera */
static u8 square_color(u8 sq)
{
    u8 rank = (u8)(sq >> 4);
    u8 file = (u8)(sq & 7);

    if (sq == h2 || sq == joyFrom) return COLOR_YELLOW;
    if (sq == joyCursor) return COLOR_CYAN;
    return (u8)(((rank + file) & 1) ? COLOR_GRAY3 : COLOR_BLUE);
}

/* disegna una casa 3x3: sfondo colorato (reverse) e pezzo "scavato" */
static void draw_square(u8 sq)
{
    u8 r, f, x, y, cr, cc, p, col, code;
    u16 sbase;

    p = lboard[sq];
    if (emptyBoard) p = 0;
    col = square_color(sq);
    r = flip ? (u8)(sq >> 4) : (u8)(7 - (sq >> 4));
    f = flip ? (u8)(7 - (sq & 7)) : (u8)(sq & 7);
    x = (u8)(BOARD_X + 3 * f);
    y = (u8)(BOARD_Y + 3 * r);
    sbase = (u16)(SCR_BASE + y * 40 + x);

    for (cr = 0; cr < 3; cr++) {
        for (cc = 0; cc < 3; cc++) {
            code = piece_code(p, cr, cc);
            POKE((u16)(sbase + cr * 40 + cc), code);
            POKE((u16)(sbase - SCR_BASE + COL_BASE + cr * 40 + cc), col);
        }
    }
}

static void draw_board(void)
{
    u8 r, f, rank, file, sq;

    scr_clear();
    scr_puts(0, 0, "ESP32CHESS", COLOR_WHITE);

    for (f = 0; f < 8; f++)
        scr_put((u8)(BOARD_X + 3 * f + 1), FILE_ROW,
                to_screen((char)('a' + (flip ? (7 - f) : f))), COLOR_WHITE);

    for (r = 0; r < 8; r++) {
        rank = flip ? r : (u8)(7 - r);
        scr_put(RANK_COL_R, (u8)(BOARD_Y + 3 * r + 1),
                to_screen((char)('1' + rank)), COLOR_WHITE);
        for (f = 0; f < 8; f++) {
            file = flip ? (u8)(7 - f) : f;
            sq = (u8)((rank << 4) | file);
            draw_square(sq);
        }
    }
}

/* ridisegna la casa sq con l'evidenziazione corrente */
static void paint_sq(u8 sq)
{
    if (sq == 0xFF) return;
    draw_square(sq);
}

/* ================================================================== */
/* timer                                                               */
/* ================================================================== */

static u32 player_ticks(void)
{
    return plBase + (segWho == SEG_PLAYER ? (u32)clock() - segT0 : 0UL);
}

static u32 ai_ticks(void)
{
    return aiBase + (segWho == SEG_AI ? (u32)clock() - segT0 : 0UL);
}

/* chiude il segmento corrente (se diverso) e apre quello di who */
static void seg_open(u8 who)
{
    if (segWho == who) return;
    if (segWho == SEG_PLAYER) plBase += (u32)clock() - segT0;
    else                      aiBase += (u32)clock() - segT0;
    segT0 = (u32)clock();
    segWho = who;
}

/* m:ss da jiffy */
static void scr_time(u8 x, u8 y, u32 j, u8 color)
{
    unsigned int m = (unsigned int)((j / 3600UL) % 100);
    char b[7];
    u8 i = 0;

    if (m >= 10) b[i++] = (char)('0' + m / 10);
    b[i++] = (char)('0' + (m % 10));
    b[i++] = ':';
    b[i++] = (char)('0' + ((j / 600UL) % 6));
    b[i++] = (char)('0' + ((j / 60UL) % 10));
    b[i] = 0;
    scr_blank(x, y, 6, COLOR_BLACK);
    scr_puts(x, y, b, color);
}

static void show_timers(void)
{
    scr_puts(0, TIME_YOU_ROW, "YOU", COLOR_WHITE);
    scr_time(4, TIME_YOU_ROW, player_ticks(), COLOR_WHITE);
    scr_puts(0, TIME_AI_ROW, "AI", COLOR_WHITE);
    scr_time(4, TIME_AI_ROW, ai_ticks(), COLOR_WHITE);
}

/* ================================================================== */
/* pannello di stato                                                   */
/* ================================================================== */

static void put_msg(const char *s, u8 color)
{
    scr_puts(0, MSG_ROW, s, color);
}

/* tiene il messaggio a schermo per ~1,5 s (con i timer aggiornati):
 * senza, il prompt successivo lo cancella prima di renderlo leggibile */
static void msg_hold(void)
{
    u32 t = (u32)clock() + 90;
    while ((u32)clock() < t) show_timers();
    scr_blank(0, MSG_ROW, PANEL_W, COLOR_BLACK);
}

static void show_status(void)
{
    scr_blank(0, MOVE_ROW, PANEL_W, COLOR_BLACK);
    scr_puts(0, MOVE_ROW, "AI:", COLOR_WHITE);
    if (hasAiMove) scr_puts(4, MOVE_ROW, aiMove, COLOR_YELLOW);

    scr_blank(0, VAL_ROW, PANEL_W, COLOR_BLACK);
    scr_puts(0, VAL_ROW, "val", COLOR_WHITE);
    if (hasAiMove) scr_snum(4, VAL_ROW, aiScore, COLOR_WHITE);

    scr_blank(0, DEPTH_ROW, PANEL_W, COLOR_BLACK);
    scr_puts(0, DEPTH_ROW, "d", COLOR_WHITE);
    if (hasAiMove) scr_unum(2, DEPTH_ROW, (unsigned int)aiDepth, COLOR_WHITE);

    if (checkFlag == 1) scr_puts(0, CHECK_ROW, "SCACCO!", COLOR_LIGHTRED);
    else if (checkFlag == 2) scr_puts(0, CHECK_ROW, "SCACCO!", COLOR_YELLOW);
    else scr_blank(0, CHECK_ROW, PANEL_W, COLOR_BLACK);
}

/* ================================================================== */
/* mossa del giocatore visibile subito                                 */
/* ================================================================== */

static void snap_save(void)
{
    memcpy(snapBoard, lboard, sizeof(lboard));
    snapLep = lep;
    snapSide = lside;
    snapH1 = h1;
    snapH2 = h2;
}

static void snap_restore(void)
{
    memcpy(lboard, snapBoard, sizeof(lboard));
    lep = snapLep;
    lside = snapSide;
    h1 = snapH1;
    h2 = snapH2;
}

/* case toccate dall'ultima ui_apply_move: servono a ripristinare il
 * disegno dopo una mossa rifiutata dall'ESP32 (solo quelle, non tutta
 * la scacchiera) */
static u8 drawnSq[6];
static u8 drawnN;

/* applica una mossa alla copia locale e ridisegna SOLO le case toccate
 * (arrocco ed en passant compresi) */
static void ui_apply_move(u16 m)
{
    u8 from = M_FROM(m);
    u8 to = M_TO(m);
    u8 p = lboard[from];
    u8 t = PT(p);
    u8 pc = PC(p);
    u8 oh2 = h2;
    u8 capture = (u8)(lboard[to] != EMPTY);
    u8 rookFrom = 0xFF, rookTo = 0xFF, epCap = 0xFF;

    if (t == KING && ((u8)(to - from) == 2 || (u8)(from - to) == 2)) {
        if (to > from) { rookFrom = (u8)(to + 1); rookTo = (u8)(to - 1); }
        else           { rookFrom = (u8)(to - 2); rookTo = (u8)(to + 1); }
    } else if (t == PAWN && lboard[to] == EMPTY && (to & 7) != (from & 7)) {
        epCap = (u8)(to + (pc == WHITE ? -16 : 16));
        capture = 1;
    }

    apply_move_local(m);
    h1 = from;
    h2 = to;
    joyFrom = 0xFF;
    joyCursor = 0xFF;
    checkFlag = 0;
    sfx_move(capture);

    drawnN = 0;
    if (oh2 != 0xFF)      drawnSq[drawnN++] = oh2;
    drawnSq[drawnN++] = from;
    drawnSq[drawnN++] = to;
    if (rookFrom != 0xFF) { drawnSq[drawnN++] = rookFrom; drawnSq[drawnN++] = rookTo; }
    if (epCap != 0xFF)    drawnSq[drawnN++] = epCap;

    for (pc = 0; pc < drawnN; pc++)      /* pc riusato come indice */
        draw_square(drawnSq[pc]);

    scr_blank(0, CHECK_ROW, PANEL_W, COLOR_BLACK);   /* via il vecchio avviso */
}

/* ================================================================== */
/* input                                                               */
/* ================================================================== */

/* "e2e4", "e2-e4", "e2 e4", "e7e8q" -> mossa 16 bit (o MOVE_NONE).
 * La promozione di default e' la donna (solo per i pedoni che promuovono). */
static u16 parse_local(const char *s) {
    u8 f0, r0, f1, r1, promo = 3, isPromo;
    u8 p;

    while (*s == ' ') s++;
    if (*s >= 'a' && *s <= 'h')      f0 = (u8)(*s - 'a');
    else if (*s >= 'A' && *s <= 'H') f0 = (u8)(*s - 'A');
    else return MOVE_NONE;
    s++;
    if (*s < '1' || *s > '8') return MOVE_NONE;
    r0 = (u8)(*s - '1');
    s++;
    if (*s == '-' || *s == ' ') s++;
    if (*s >= 'a' && *s <= 'h')      f1 = (u8)(*s - 'a');
    else if (*s >= 'A' && *s <= 'H') f1 = (u8)(*s - 'A');
    else return MOVE_NONE;
    s++;
    if (*s < '1' || *s > '8') return MOVE_NONE;
    r1 = (u8)(*s - '1');
    s++;
    if (*s == '-' || *s == ' ') s++;
    if (*s) {
        if (*s == 'q' || *s == 'Q')      promo = 3;
        else if (*s == 'n' || *s == 'N') promo = 0;
        else if (*s == 'b' || *s == 'B') promo = 1;
        else if (*s == 'r' || *s == 'R') promo = 2;
        else return MOVE_NONE;
    }

    p = lboard[(u8)((r0 << 4) | f0)];
    isPromo = (u8)(PT(p) == PAWN && (r1 == 0 || r1 == 7));
    if (!isPromo) promo = 0;

    return MK_MOVE((u8)((r0 << 4) | f0),
                   (u8)((r1 << 4) | f1),
                   promo);
}

/* ------------------------------------------------------------------ */
/* joystick                                                            */
/* ------------------------------------------------------------------ */

#define JB_UP    0x01
#define JB_DOWN  0x02
#define JB_LEFT  0x04
#define JB_RIGHT 0x08
#define JB_FIRE  0x10

static u8 joy_read_raw(void)
{
    u8 p2 = (u8)((PEEK(0xDC00) ^ 0x1F) & 0x1F);
    u8 p1 = (u8)((PEEK(0xDC01) ^ 0x1F) & 0x1F);
    return p2 ? p2 : p1;   /* 1 = premuto */
}

static u8 joy_poll(void)
{
    static u8 stable = 0;       /* ultimo stato stabile (debounced) */
    static u8 cand = 0;         /* stato in stabilizzazione */
    static u8 candCnt = 0;
    static u8 prev = 0;
    static u32 lastTick = 0;
    static u32 holdT0 = 0;      /* inizio della tenuta dello stato stabile */
    static u32 lastRep = 0;     /* ultimo auto-repeat */
    u32 now = (u32)clock();
    u8 raw = joy_read_raw();
    u8 e;

    if (raw != cand) {
        cand = raw;
        candCnt = 0;
    }
    if (now != lastTick) {
        lastTick = now;
        if (candCnt < 2) candCnt++;
        if (candCnt == 2 && stable != cand) {
            stable = cand;      /* nuovo stato stabile: reset del repeat */
            holdT0 = now;
            lastRep = now;
        }
    }

    e = (u8)(stable & (stable ^ prev));   /* press: nuovo evento */
    prev = stable;
    if (e) return e;

    /* auto-repeat delle direzioni se tenute (non con il fire premuto):
     * dopo ~1/4 s di tenuta, un passo ogni ~2 jiffy */
    if (!(stable & JB_FIRE) && (stable & 0x0F) &&
        now - holdT0 > 15 && now - lastRep >= 2) {
        lastRep = now;
        return (u8)(stable & 0x0F);
    }
    return 0;
}

static u16 move_from_squares(u8 from, u8 to)
{
    char s[6];

    s[0] = (char)('a' + (from & 7));
    s[1] = (char)('1' + (from >> 4));
    s[2] = (char)('a' + (to & 7));
    s[3] = (char)('1' + (to >> 4));
    s[4] = 'q';                 /* promozione default: la valida l'ESP32 */
    s[5] = 0;
    return parse_local(s);
}

/* input della mossa umana: tastiera (e2e4) oppure joystick (cursore).
   Ritorna la mossa scelta, MOVE_UNDO per 'u', MOVE_NONE per uscire. */
static u16 get_human_move(void)
{
    char kb[8];
    u8 klen = 0;
    u8 r, f, sq, e;
    u32 lastJfy;
    int c;

    scr_blank(0, PROMPT_ROW, PANEL_W, COLOR_BLACK);
    scr_blank(0, MSG_ROW, PANEL_W, COLOR_BLACK);
    scr_puts(0, PROMPT_ROW, "move:", COLOR_WHITE);

    joyCursor = find_king(lside);
    joyFrom = 0xFF;
    paint_sq(joyCursor);

    lastJfy = (u32)clock();
    for (;;) {
        if ((u32)clock() != lastJfy) {      /* timer giocatore live */
            lastJfy = (u32)clock();
            show_timers();
        }

        if (kbhit()) {
            c = cgetc();
            if (c == 13 || c == 10) {
                u16 mv;
                if (klen == 0) continue;
                kb[klen] = 0;
                if (klen == 1 && (kb[0] == 'u' || kb[0] == 'U'))
                    return MOVE_UNDO;
                mv = parse_local(kb);
                if (mv != MOVE_NONE) return mv;
                put_msg("bad move", COLOR_LIGHTRED);
                sfx_error();
                while (klen) {
                    klen--;
                    scr_put((u8)(6 + klen), PROMPT_ROW, 0x20, COLOR_WHITE);
                }
                continue;
            }
            if (c == 20 || c == 8) {
                if (klen) {
                    klen--;
                    scr_put((u8)(6 + klen), PROMPT_ROW, 0x20, COLOR_WHITE);
                }
                continue;
            }
            if (c == 3 || c == '.') return MOVE_NONE;
            if (klen < 5) {
                scr_put((u8)(6 + klen), PROMPT_ROW, to_screen((char)c), COLOR_WHITE);
                kb[klen++] = (char)c;
            }
        } else {
            e = joy_poll();
            if (!e) continue;

            r = (u8)(joyCursor >> 4);
            f = (u8)(joyCursor & 7);
            if ((e & JB_UP)    && (flip ? r > 0 : r < 7)) r = (u8)(r + (flip ? -1 : 1));
            if ((e & JB_DOWN)  && (flip ? r < 7 : r > 0)) r = (u8)(r + (flip ? 1 : -1));
            if ((e & JB_LEFT)  && (flip ? f < 7 : f > 0)) f = (u8)(f + (flip ? 1 : -1));
            if ((e & JB_RIGHT) && (flip ? f > 0 : f < 7)) f = (u8)(f + (flip ? -1 : 1));
            sq = (u8)((r << 4) | f);

            if (sq != joyCursor) {
                u8 old = joyCursor;
                joyCursor = sq;
                paint_sq(old);
                paint_sq(joyCursor);
            }

            if (e & JB_FIRE) {
                if (joyFrom == 0xFF) {
                    if (lboard[joyCursor] && PC(lboard[joyCursor]) == lside) {
                        joyFrom = joyCursor;
                        paint_sq(joyFrom);
                    } else {
                        put_msg("not yours", COLOR_LIGHTRED);
                        sfx_error();
                    }
                } else {
                    u16 mv = move_from_squares(joyFrom, joyCursor);
                    if (mv != MOVE_NONE) return mv;
                    put_msg("bad move", COLOR_LIGHTRED);
                    sfx_error();
                    {
                        u8 oldFrom = joyFrom;
                        joyFrom = 0xFF;
                        if (lboard[joyCursor] && PC(lboard[joyCursor]) == lside)
                            joyFrom = joyCursor;
                        if (oldFrom != joyCursor) paint_sq(oldFrom);
                        paint_sq(joyCursor);
                    }
                }
            }
        }
    }
}

/* ================================================================== */
/* protocollo                                                          */
/* ================================================================== */

/* Errore di collegamento: svuota il buffer e segnala; il chiamante decide
 * se provare a risincronizzare o uscire. */
static void link_error(void) {
    comm_rx_flush();
    scr_blank(0, PROMPT_ROW, PANEL_W, COLOR_BLACK);
    scr_blank(0, MSG_ROW, PANEL_W, COLOR_BLACK);
    put_msg("LINK ERROR", COLOR_LIGHTRED);
    scr_puts(0, OVER_KEY_ROW, "press a key", COLOR_WHITE);
    (void)cgetc();
    scr_blank(0, OVER_KEY_ROW, PANEL_W, COLOR_BLACK);
}

/* Consuma i byte residui finche' non arriva silenzio per un breve timeout:
 * dopo un errore l'ESP32 puo' avere una raffica ancora in volo. */
static void drain_link(void) {
    while (comm_rx_byte(100) >= 0) ;
}

/* Ritorna 1 se e' arrivato l'ACK, 0 su timeout/errore. La consegna dei
 * comandi e' affidabile (tx con ack): se l'ACK si perde il comando e'
 * comunque stato eseguito dall'ESP32. */
static unsigned char wait_ack(void) {
    return (comm_rx_byte(RX_TIMEOUT_CMD) == RSP_ACK) ? 1 : 0;
}

/* Ritorna 0 ok, -1 su errore di trasmissione. */
static int send_move(u16 m) {
    if (comm_tx_byte(CMD_MOVE) != 0) return -1;
    if (comm_tx_byte((unsigned char)(m & 0xFF)) != 0) return -1;
    if (comm_tx_byte((unsigned char)((m >> 8) & 0xFF)) != 0) return -1;
    return 0;
}

/* Attende il primo byte di risposta aggiornando il timer dell'AI; usato
 * quando l'ESP32 puo' impiegarci il tempo del think. Ritorna il byte,
 * -1 su deadline scaduta.
 * Usa comm_rx_poll (mai il flush di comm_rx_byte): il flush ripetuto ad
 * ogni timeout puo' buttare via il byte che arriva proprio nella finestra
 * tra l'ultimo controllo e lo svuotamento (~30us per poll) ed era la
 * causa di LINK ERROR sporadici a partita avanzata. */
static int wait_rsp(void) {
    u32 deadline = (u32)clock() + MS_TO_JFY(RX_TIMEOUT_CMD);
    u32 lastJfy = (u32)clock();
    int r;

    scr_blank(0, PROMPT_ROW, PANEL_W, COLOR_BLACK);
    scr_puts(0, PROMPT_ROW, "thinking...", COLOR_WHITE);
    for (;;) {
        r = comm_rx_poll();
        if (r >= 0) return r;
        if ((u32)clock() != lastJfy) {
            lastJfy = (u32)clock();
            show_timers();
            if (lastJfy >= deadline) return -1;
        }
    }
}

/* Legge reason/winner di GAME_OVER. Ritorna 1 ok, 0 su timeout. */
static unsigned char read_over_tail(unsigned char *reason, unsigned char *winner) {
    int r = comm_rx_byte(RX_TIMEOUT_BYTE);
    int w = comm_rx_byte(RX_TIMEOUT_BYTE);
    if (r < 0 || w < 0) { link_error(); return 0; }
    *reason = (unsigned char)r;
    *winner = (unsigned char)w;
    return 1;
}

/* Mostra il risultato e chiede cosa fare; esita in g_wantNew (1 = nuova
 * partita). Il programma NON esce piu' in silenzio a BASIC dopo la partita. */
static void show_game_over(u8 reason, u8 winner) {
    static const char *rtxt[5] = {
        "SCACCO MATTO", "STALLO", "50 MOSSE", "RIPETIZIONE", "MATERIALE"
    };
    char c;

    sfx_gameover();
    scr_blank(0, PROMPT_ROW, PANEL_W, COLOR_BLACK);
    scr_blank(0, MSG_ROW, PANEL_W, COLOR_BLACK);
    scr_blank(0, OVER_ROW, PANEL_W, COLOR_BLACK);
    scr_blank(0, OVER_WHO_ROW, PANEL_W, COLOR_BLACK);
    scr_blank(0, OVER_KEY_ROW, PANEL_W, COLOR_BLACK);

    if (reason >= 1 && reason <= 5)
        scr_puts(0, OVER_ROW, rtxt[reason - 1], COLOR_YELLOW);
    if (reason == OVER_MATE)
        scr_puts(0, OVER_WHO_ROW,
                 winner == WINNER_WHITE ? "BIANCO VINCE" : "NERO VINCE", COLOR_YELLOW);
    else
        scr_puts(0, OVER_WHO_ROW, "PATTA", COLOR_YELLOW);
    scr_puts(0, OVER_KEY_ROW, "N=NUOVA Q=ESCI", COLOR_WHITE);

    for (;;) {
        c = cgetc();
        if (c == 'n' || c == 'N') { g_wantNew = 1; return; }
        if (c == 'q' || c == 'Q') { g_wantNew = 0; return; }
    }
}

/* Legge il payload di BEST_MOVE (il byte RSP_BEST_MOVE e' gia' stato letto),
 * applica la mossa del computer e aggiorna pannello e timer.
 * NOTA: la mossa dell'umano e' gia' stata applicata e mostrata: qui si
 * applica SOLO la mossa del computer.
 * Ritorna 1 se la partita e' finita, 0 se prosegue, 2 su errore di link. */
static unsigned char recv_best_payload(void) {
    u16 m;
    int b, score;
    unsigned char depth, flags, reason, winner;

    b = comm_rx_byte(RX_TIMEOUT_BYTE); if (b < 0) { link_error(); return 2; }
    m = (u16)b;
    b = comm_rx_byte(RX_TIMEOUT_BYTE); if (b < 0) { link_error(); return 2; }
    m |= (u16)b << 8;
    b = comm_rx_byte(RX_TIMEOUT_BYTE); if (b < 0) { link_error(); return 2; }
    score = b;
    b = comm_rx_byte(RX_TIMEOUT_BYTE); if (b < 0) { link_error(); return 2; }
    score |= b << 8;
    b = comm_rx_byte(RX_TIMEOUT_BYTE); if (b < 0) { link_error(); return 2; }
    depth = (unsigned char)b;
    b = comm_rx_byte(RX_TIMEOUT_BYTE); if (b < 0) { link_error(); return 2; }
    flags = (unsigned char)b;

    aiScore = (s16)score;
    aiDepth = depth;
    checkFlag = (u8)(flags & F_CHECK);

    ui_apply_move(m);
    if (histN < 400) hist[histN++] = m;

    aiMove[0] = (char)('a' + (M_FROM(m) & 7));
    aiMove[1] = (char)('1' + (M_FROM(m) >> 4));
    aiMove[2] = '-';
    aiMove[3] = (char)('a' + (M_TO(m) & 7));
    aiMove[4] = (char)('1' + (M_TO(m) >> 4));
    aiMove[5] = 0;
    hasAiMove = 1;

    show_status();
    show_timers();
    if (checkFlag) sfx_check();

    if (flags & F_GAMEOVER) {
        if (!read_over_tail(&reason, &winner)) return 2;
        show_game_over(reason, winner);
        return 1;
    }
    return 0;
}

/* Legge la risposta del computer a NEW_GAME/GO (dopo l'eventuale ACK).
 * Ritorna 0 ok, 1 partita finita (show_game_over ha chiesto nuova/esci),
 * 2 errore di link. */
static unsigned char recv_engine_move(void) {
    int r;
    unsigned char reason, winner;

    r = wait_rsp();
    if (r < 0) { link_error(); return 2; }
    scr_blank(0, PROMPT_ROW, PANEL_W, COLOR_BLACK);
    if (r == RSP_BEST_MOVE) return recv_best_payload();
    if (r == RSP_GAME_OVER) {
        if (!read_over_tail(&reason, &winner)) return 2;
        show_game_over(reason, winner);
        return 1;
    }
    link_error();
    return 2;
}

/* Manda NEW_GAME e, se l'umano e' nero, consuma l'apertura del computer.
 * Ricostruisce anche la board locale e azzera timer e pannello.
 * Ritorna 0 ok, 1 partita finita subito, 2 errore di link. */
static unsigned char start_new_game(void) {
    histN = 0;
    new_board_local();
    h1 = h2 = 0xFF;
    hasAiMove = 0;
    checkFlag = 0;
    joyFrom = 0xFF;
    joyCursor = 0xFF;
    plBase = aiBase = 0;
    segT0 = (u32)clock();
    segWho = SEG_PLAYER;
    draw_board();

    drain_link();
    if (comm_tx_byte(CMD_NEW_GAME) != 0) { link_error(); return 2; }
    (void)wait_ack();
    if (humanColor != BLACK) return 0;
    return recv_engine_move();
}

/* Riallinea la UI allo stato vero dell'ESP32 senza buttare la partita:
 * (ri)invia colore e livello (copre anche un reboot dell'ESP32), chiede lo
 * storico mosse (GET_HIST) e ricostruisce la board locale; se dopo il resync
 * tocca al computer lo fa muovere (GO).
 * Ritorna 1 se la partita puo' continuare, 0 su errore di link. */
static unsigned char resync_board(void) {
    u16 n, i, m;
    int b;

    drain_link();
    if (comm_tx_byte(CMD_SET_COLOR) != 0 ||
        comm_tx_byte(humanColor) != 0) return 0;
    (void)wait_ack();
    if (comm_tx_byte(CMD_SET_LEVEL) != 0 ||
        comm_tx_byte(level) != 0) return 0;
    (void)wait_ack();
    if (comm_tx_byte(CMD_GET_HIST) != 0) return 0;

    b = comm_rx_byte(RX_TIMEOUT_BYTE);
    if (b != RSP_HIST) { link_error(); return 0; }
    b = comm_rx_byte(RX_TIMEOUT_BYTE); if (b < 0) { link_error(); return 0; }
    n = (u16)b;
    b = comm_rx_byte(RX_TIMEOUT_BYTE); if (b < 0) { link_error(); return 0; }
    n |= (u16)b << 8;
    if (n > 400) { link_error(); return 0; }

    histN = 0;
    new_board_local();
    for (i = 0; i < n; i++) {
        b = comm_rx_byte(RX_TIMEOUT_BYTE); if (b < 0) { link_error(); return 0; }
        m = (u16)b;
        b = comm_rx_byte(RX_TIMEOUT_BYTE); if (b < 0) { link_error(); return 0; }
        m |= (u16)b << 8;
        apply_move_local(m);
        hist[histN++] = m;
    }
    if (histN) {
        h1 = M_FROM(hist[histN - 1]);
        h2 = M_TO(hist[histN - 1]);
    } else {
        h1 = h2 = 0xFF;
    }
    hasAiMove = 0;
    checkFlag = 0;
    joyFrom = 0xFF;
    joyCursor = 0xFF;
    draw_board();
    show_status();
    show_timers();

    if (lside != humanColor) {            /* tocca al computer: falla muovere */
        if (comm_tx_byte(CMD_GO) != 0) { link_error(); return 0; }
        if (recv_engine_move() == 2) return 0;
    }
    return 1;
}

/* Ritorna 0 ok, 1 se il collegamento e' morto (game_loop deve uscire). */
static unsigned char do_undo(void) {
    u16 i;
    if (histN < 2) {
        put_msg("nothing", COLOR_LIGHTRED);
        sfx_error();
        return 0;
    }
    if (comm_tx_byte(CMD_UNDO) != 0) {
        link_error();
        return resync_board() ? 0 : 1;
    }
    (void)wait_ack();

    histN -= 2;
    new_board_local();
    for (i = 0; i < histN; i++) apply_move_local(hist[i]);
    if (histN) {
        h1 = M_FROM(hist[histN - 1]);
        h2 = M_TO(hist[histN - 1]);
    } else {
        h1 = h2 = 0xFF;
    }
    hasAiMove = 0;
    checkFlag = 0;
    draw_board();
    show_status();
    show_timers();
    return 0;
}

/* ================================================================== */
/* loop di gioco                                                       */
/* ================================================================== */

static void game_loop(void) {
    u16 m;
    u8 from, p, reason, winner, rc;
    int rsp;

    for (;;) {
        show_timers();
        seg_open(SEG_PLAYER);
        m = get_human_move();

        if (m == MOVE_NONE) {                 /* '.' o RUN-STOP: esci */
            if (comm_tx_byte(CMD_QUIT) == 0) (void)wait_ack();
            return;
        }
        if (m == MOVE_UNDO) {                 /* 'u': annulla l'ultimo turno */
            if (do_undo()) return;
            continue;
        }

        from = M_FROM(m);
        p = lboard[from];
        if (p == EMPTY || PC(p) != lside) {
            put_msg("not yours", COLOR_LIGHTRED);
            sfx_error();
            msg_hold();
            continue;
        }

        snap_save();
        ui_apply_move(m);          /* mossa del giocatore visibile subito */
        comm_rx_flush();
        if (send_move(m) != 0) {   /* l'ESP32 non ha confermato: resync */
            link_error();
            if (!resync_board()) return;
            continue;
        }

        seg_open(SEG_AI);          /* il tempo d'attesa e' tempo dell'AI */
        /* la conferma della mossa (MOVED) e l'eventuale rifiuto arrivano
         * subito; con firmware vecchio (senza MOVED) si passa dritti a
         * BEST_MOVE/GAME_OVER */
        rsp = comm_rx_byte(RX_TIMEOUT_BYTE);

        if (rsp == RSP_MOVED) {
            int f = comm_rx_byte(RX_TIMEOUT_BYTE);
            if (f < 0) {
                link_error();
                if (!resync_board()) return;
                continue;
            }
            if (f & F_CHECK) {                /* la tua mossa ha dato scacco */
                checkFlag = 2;
                show_status();
                sfx_check();
            }
            rsp = wait_rsp();                 /* poi GAME_OVER o BEST_MOVE */
        }

        if (rsp == RSP_ILLEGAL) {
            u8 i;
            snap_restore();
            for (i = 0; i < drawnN; i++)   /* ridisegna solo le case toccate */
                draw_square(drawnSq[i]);
            scr_blank(0, PROMPT_ROW, PANEL_W, COLOR_BLACK);
            put_msg("bad move", COLOR_LIGHTRED);
            sfx_error();
            msg_hold();
            continue;
        }
        if (rsp == RSP_GAME_OVER) {           /* l'umano ha chiuso la partita */
            if (histN < 400) hist[histN++] = m;
            if (!read_over_tail(&reason, &winner)) return;
            show_game_over(reason, winner);
            if (!g_wantNew || start_new_game() != 0) return;
            continue;
        }
        if (rsp == RSP_BEST_MOVE) {           /* partita che prosegue */
            if (histN < 400) hist[histN++] = m;
            rc = recv_best_payload();
            if (rc == 2) {
                link_error();
                if (!resync_board()) return;
                continue;
            }
            if (rc == 1) {          /* show_game_over ha chiesto nuova/esci */
                if (!g_wantNew || start_new_game() != 0) return;
            }
            continue;
        }
        /* risposta ignota o assente: protocollo sfasato, non prosegue a vuoto */
        link_error();
        if (!resync_board()) return;
    }
}

/* ================================================================== */
/* menu                                                                */
/* ================================================================== */

static u8 ask_level(void)
{
    int c;

    scr_puts(0, 2, "LEVEL", COLOR_WHITE);
    scr_puts(0, 4, "1 EASY", COLOR_WHITE);
    scr_puts(0, 5, "2 MEDIUM", COLOR_WHITE);
    scr_puts(0, 6, "3 HARD", COLOR_WHITE);
    scr_puts(0, 7, "4 STRONG", COLOR_WHITE);
    scr_puts(0, 9, "CHOICE:", COLOR_WHITE);
    do { c = cgetc(); } while (c < '1' || c > '4');
    scr_put(7, 9, to_screen((char)c), COLOR_WHITE);
    return (u8)(c - '1');
}

static u8 ask_color(void)
{
    int c;

    scr_puts(0, 11, "COLOR:", COLOR_WHITE);
    scr_puts(0, 12, "1 WHITE", COLOR_WHITE);
    scr_puts(0, 13, "2 BLACK", COLOR_WHITE);
    do { c = cgetc(); } while (c < '1' || c > '2');
    scr_put(7, 11, to_screen((char)c), COLOR_WHITE);
    return (u8)(c - '1');
}

/* ================================================================== */

int main(void)
{
    u8 rc;

    POKE(0xD020, COLOR_BLACK);               /* bordo */
    POKE(0xD021, COLOR_BLACK);               /* sfondo */

    comm_install();                          /* handler NMI al piu' presto */
    install_charset();
    POKE(0xDD00, (u8)((PEEK(0xDD00) & 0xFC) | 0x01));  /* banca VIC 2 */
    POKE(0xD018, 0x13);                      /* schermo $8400, charset $8800 */

    new_board_local();
    humanColor = WHITE;
    level = LEVEL_MEDIUM;
    h1 = h2 = 0xFF;
    joyCursor = 0xFF;
    joyFrom = 0xFF;
    hasAiMove = 0;
    segWho = SEG_PLAYER;
    segT0 = (u32)clock();

    emptyBoard = 1;
    draw_board();
    emptyBoard = 0;

    level = ask_level();
    humanColor = ask_color();
    flip = (humanColor == BLACK) ? 1 : 0;

    draw_board();

    comm_rx_flush();
    if (comm_tx_byte(CMD_SET_COLOR) != 0 || comm_tx_byte(humanColor) != 0 ||
        comm_tx_byte(CMD_SET_LEVEL) != 0 || comm_tx_byte(level) != 0) {
        link_error();
        return 1;
    }
    (void)wait_ack();
    (void)wait_ack();

    rc = start_new_game();
    while (rc == 1) {           /* partita finita subito: la scelta nuova/esci
                                   e' gia' stata chiesta da show_game_over */
        if (!g_wantNew) return 0;
        rc = start_new_game();
    }
    if (rc == 2) return 1;

    game_loop();
    return 0;
}
