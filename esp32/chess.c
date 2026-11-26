/*
 * chess.c - motore scacchi: board, generazione mosse legali, valutazione,
 *           ricerca alpha-beta con quiescenza, regole di patta.
 *
 * Scelte pensate per cc65/C64:
 *   - board 0x88 da 128 byte (casella fuori scacchiera: sq & 0x88)
 *   - mosse da 16 bit, nessuna struct nelle liste
 *   - nessun float, nessun long nella ricerca (solo nei contatori)
 *   - liste di mosse e undo statici, indicizzati dal ply
 */

#include "chess.h"
#include <string.h>
#include <time.h>

u8  board[128];
u8  side;
u8  castle;
s8  epSq;
u8  halfmove;
u8  kingSq[2];
u16 posKey;
u8  gamePly;
u16 keyHist[MAXKEYS];
u32 nodes;

static u8  histPly;                       /* ply della ricerca in corso */
static u16 moveBuf[MAXPLY][MAXMOVES];
static s16 scoreBuf[MAXPLY][MAXMOVES];

typedef struct {
    s16 eval;
    s16 npm;
    u8  captured;
    u8  capSq;
    u8  moved;
    u8  castle;
    s8  ep;
    u8  halfmove;
    u8  rookFrom;
    u8  rookTo;
    u16 key;
} Undo;

static Undo undos[MAXSEARCH];

/* materiale + tabelle posizionali, mantenuto in modo incrementale
   (dal punto di vista del bianco). Azzera il costo della evaluate(). */
static s16 evalWhite;
static s16 npm;          /* materiale non-pedone totale: serve per il finale */

/* ------------------------------------------------------------------ */
/* tabelle                                                             */
/* ------------------------------------------------------------------ */

static u16 zPiece[15][64];
static u16 zCastle[16];
static u16 zEp[8];
static u16 zSide;
static u8  sq64[128];
static u8  pstIdx[2][128];

/* stato del re calcolato una volta per nodo */
static u8 pinKind[128];       /* 0 = libero, 1 = colonna, 2 = traversa,
                                 3 = diagonale \\, 4 = diagonale / */
static u8 pinStamp[128];      /* token del nodo che ha scritto pinKind */
static u8 pinToken;           /* token del nodo corrente */
static u8 inCheckFlag;

static const s16 pieceVal[7] = { 0, 100, 320, 330, 500, 900, 0 };

static const s8 qOff[8] = { 17, 16, 15, 1, -1, -15, -16, -17 };

static const s8 promoType[4] = { KNIGHT, BISHOP, ROOK, QUEEN };

/* tipo di linea per ogni direzione di qOff:
   0=+17 \\\\ , 1=+16 |, 2=+15 /, 3=+1 -, 4=-1 -, 5=-15 /, 6=-16 |, 7=-17 \\\\ */
static const u8 pinKindOfDir[8] = { 3, 1, 4, 2, 2, 4, 1, 3 };
/* 1 = direzione ortogonale (torre/donna), 0 = diagonale (alfiere/donna) */
static const u8 orthoDir[8] = { 0, 1, 0, 1, 1, 0, 1, 0 };

static const u8 castleMask[128] = {
    13, 15, 15, 15, 12, 15, 15, 14,  15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,  15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,  15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,  15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,  15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,  15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,  15, 15, 15, 15, 15, 15, 15, 15,
     7, 15, 15, 15,  3, 15, 15, 11,  15, 15, 15, 15, 15, 15, 15, 15
};
/* tabelle posizionali: riga 0 = traversa 8 (prospettiva bianco) */
static const s8 pstPawn[64] = {
      0,  0,  0,  0,  0,  0,  0,  0,
     50, 50, 50, 50, 50, 50, 50, 50,
     10, 10, 20, 30, 30, 20, 10, 10,
      5,  5, 10, 25, 25, 10,  5,  5,
      0,  0,  0, 20, 20,  0,  0,  0,
      5, -5,-10,  0,  0,-10, -5,  5,
      5, 10, 10,-20,-20, 10, 10,  5,
      0,  0,  0,  0,  0,  0,  0,  0
};

static const s8 pstKnight[64] = {
    -50,-40,-30,-30,-30,-30,-40,-50,
    -40,-20,  0,  0,  0,  0,-20,-40,
    -30,  0, 10, 15, 15, 10,  0,-30,
    -30,  5, 15, 20, 20, 15,  5,-30,
    -30,  0, 15, 20, 20, 15,  0,-30,
    -30,  5, 10, 15, 15, 10,  5,-30,
    -40,-20,  0,  5,  5,  0,-20,-40,
    -50,-40,-30,-30,-30,-30,-40,-50
};

static const s8 pstBishop[64] = {
    -20,-10,-10,-10,-10,-10,-10,-20,
    -10,  0,  0,  0,  0,  0,  0,-10,
    -10,  0,  5, 10, 10,  5,  0,-10,
    -10,  5,  5, 10, 10,  5,  5,-10,
    -10,  0, 10, 10, 10, 10,  0,-10,
    -10, 10, 10, 10, 10, 10, 10,-10,
    -10,  5,  0,  0,  0,  0,  5,-10,
    -20,-10,-10,-10,-10,-10,-10,-20
};

static const s8 pstRook[64] = {
      0,  0,  0,  0,  0,  0,  0,  0,
      5, 10, 10, 10, 10, 10, 10,  5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
      0,  0,  0,  5,  5,  0,  0,  0
};

static const s8 pstQueen[64] = {
    -20,-10,-10, -5, -5,-10,-10,-20,
    -10,  0,  0,  0,  0,  0,  0,-10,
    -10,  0,  5,  5,  5,  5,  0,-10,
     -5,  0,  5,  5,  5,  5,  0, -5,
      0,  0,  5,  5,  5,  5,  0, -5,
    -10,  5,  5,  5,  5,  5,  0,-10,
    -10,  0,  5,  0,  0,  0,  0,-10,
    -20,-10,-10, -5, -5,-10,-10,-20
};

static const s8 pstKing[64] = {
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -20,-30,-30,-40,-40,-30,-30,-20,
    -10,-20,-20,-20,-20,-20,-20,-10,
     20, 20,  0,  0,  0,  0, 20, 20,
     20, 30, 10,  0,  0, 10, 30, 20
};

static const s8 pstKingEnd[64] = {
    -50,-40,-30,-20,-20,-30,-40,-50,
    -30,-20,-10,  0,  0,-10,-20,-30,
    -30,-10, 20, 30, 30, 20,-10,-30,
    -30,-10, 30, 40, 40, 30,-10,-30,
    -30,-10, 30, 40, 40, 30,-10,-30,
    -30,-10, 20, 30, 30, 20,-10,-30,
    -30,-30,  0,  0,  0,  0,-30,-30,
    -50,-30,-30,-30,-30,-30,-30,-50
};

static const s8 *pst_tab(u8 t)
{
    switch (t) {
        case PAWN:   return pstPawn;
        case KNIGHT: return pstKnight;
        case BISHOP: return pstBishop;
        case ROOK:   return pstRook;
        case QUEEN:  return pstQueen;
        default:     return pstKing;
    }
}

/* ------------------------------------------------------------------ */
/* inizializzazione                                                    */
/* ------------------------------------------------------------------ */

void init_engine(void)
{
    u8  t, sq, rank, file, q;
    u16 s = 0x1234;

    for (t = 0; t < 15; t++)
        for (sq = 0; sq < 64; sq++) {
            s = (u16)(s * 25173u + 13849u);
            zPiece[t][sq] = s;
        }
    for (t = 0; t < 16; t++) {
        s = (u16)(s * 25173u + 13849u);
        zCastle[t] = s;
    }
    for (t = 0; t < 8; t++) {
        s = (u16)(s * 25173u + 13849u);
        zEp[t] = s;
    }
    s = (u16)(s * 25173u + 13849u);
    zSide = s;

    for (rank = 0; rank < 8; rank++)
        for (file = 0; file < 8; file++) {
            sq = (u8)((rank << 4) | file);
            q = (u8)((rank << 3) | file);
            sq64[sq] = q;
            pstIdx[WHITE][sq] = (u8)(((7 - rank) << 3) | file);
            pstIdx[BLACK][sq] = q;
        }
}

void new_game(void)
{
    u8 i;

    memset(board, 0, sizeof(board));
    board[0]  = MAKE_PIECE(ROOK, WHITE);
    board[1]  = MAKE_PIECE(KNIGHT, WHITE);
    board[2]  = MAKE_PIECE(BISHOP, WHITE);
    board[3]  = MAKE_PIECE(QUEEN, WHITE);
    board[4]  = MAKE_PIECE(KING, WHITE);
    board[5]  = MAKE_PIECE(BISHOP, WHITE);
    board[6]  = MAKE_PIECE(KNIGHT, WHITE);
    board[7]  = MAKE_PIECE(ROOK, WHITE);

    board[112] = MAKE_PIECE(ROOK, BLACK);
    board[113] = MAKE_PIECE(KNIGHT, BLACK);
    board[114] = MAKE_PIECE(BISHOP, BLACK);
    board[115] = MAKE_PIECE(QUEEN, BLACK);
    board[116] = MAKE_PIECE(KING, BLACK);
    board[117] = MAKE_PIECE(BISHOP, BLACK);
    board[118] = MAKE_PIECE(KNIGHT, BLACK);
    board[119] = MAKE_PIECE(ROOK, BLACK);

    for (i = 0; i < 8; i++) {
        board[16 + i] = MAKE_PIECE(PAWN, WHITE);
        board[96 + i] = MAKE_PIECE(PAWN, BLACK);
    }

    side = WHITE;
    castle = C_WK | C_WQ | C_BK | C_BQ;
    epSq = -1;
    halfmove = 0;
    kingSq[WHITE] = 4;
    kingSq[BLACK] = 116;
    refresh_eval();
    compute_key();
    reset_history();
}

void reset_history(void)
{
    gamePly = 0;
    histPly = 0;
    keyHist[0] = posKey;
}

void compute_key(void)
{
    u8 sq, p;

    posKey = 0;
    for (sq = 0; sq < 128; sq++) {
        if (sq & 0x88) continue;
        p = board[sq];
        if (p) posKey ^= zPiece[p][sq64[sq]];
    }
    posKey ^= zCastle[castle];
    if (epSq >= 0) posKey ^= zEp[epSq & 7];
    if (side == BLACK) posKey ^= zSide;
}

/* ------------------------------------------------------------------ */
/* attacco                                                             */
/* ------------------------------------------------------------------ */

static u8 attacked(u8 sq, u8 by)
{
    VLOCAL u8 s, p;

    /* pedoni: un pedone bianco attacca sq da sq-15/sq-17 */
    if (by == WHITE) {
        s = (u8)(sq - 15);
        if (!(s & 0x88) && board[s] == MAKE_PIECE(PAWN, WHITE)) return 1;
        s = (u8)(sq - 17);
        if (!(s & 0x88) && board[s] == MAKE_PIECE(PAWN, WHITE)) return 1;
    } else {
        s = (u8)(sq + 15);
        if (!(s & 0x88) && board[s] == MAKE_PIECE(PAWN, BLACK)) return 1;
        s = (u8)(sq + 17);
        if (!(s & 0x88) && board[s] == MAKE_PIECE(PAWN, BLACK)) return 1;
    }

/* il cavallo e il re si controllano con offset costanti srotolati:
   su 6502 costa meno di una tabella indicizzata (niente moltipliche) */
#define CHK(O,T) { s = (u8)(sq + (O)); \
        if (!(s & 0x88) && board[s] == MAKE_PIECE((T), by)) return 1; }
    CHK(33, KNIGHT) CHK(31, KNIGHT) CHK(18, KNIGHT) CHK(14, KNIGHT)
    CHK(-33, KNIGHT) CHK(-31, KNIGHT) CHK(-18, KNIGHT) CHK(-14, KNIGHT)
    CHK(17, KING) CHK(16, KING) CHK(15, KING) CHK(1, KING)
    CHK(-1, KING) CHK(-15, KING) CHK(-16, KING) CHK(-17, KING)
#undef CHK

#define RAY(O,T1,T2) \
    s = (u8)(sq + (O)); \
    while (!(s & 0x88)) { \
        p = board[s]; \
        if (p) { \
            if (PC(p) == by && (PT(p) == (T1) || PT(p) == (T2))) return 1; \
            break; \
        } \
        s = (u8)(s + (O)); \
    }
    RAY(17, BISHOP, QUEEN) RAY(15, BISHOP, QUEEN)
    RAY(-15, BISHOP, QUEEN) RAY(-17, BISHOP, QUEEN)
    RAY(16, ROOK, QUEEN) RAY(-16, ROOK, QUEEN)
    RAY(1, ROOK, QUEEN) RAY(-1, ROOK, QUEEN)
#undef RAY
    return 0;
}

u8 in_check(void)
{
    return attacked(kingSq[side], (u8)(side ^ 1));
}

/* ------------------------------------------------------------------ */
/* generazione mosse                                                   */
/* ------------------------------------------------------------------ */

/* NB: su cc65 le locali finiscono sullo stack software e ogni accesso
   diventa un (sp),y: per le funzioni calde conviene tenerle in BSS.
   gen_moves non e' ricorsiva, quindi e' sicuro. */
static u8 gm_n;
#ifndef USE_ASM_MOVEGEN
static u8 gm_rank, gm_file, gm_sq, gm_p, gm_t, gm_c, gm_to;
#endif

/* add_move come macro: su 6502 la chiamata costava piu' dello store */
#if MAXMOVES < 256
#define ADD_MOVE(mv) do { if ((u16)gm_n < (u16)MAXMOVES) { list[gm_n] = (mv); gm_n++; } } while (0)
#else
#define ADD_MOVE(mv) do { list[gm_n] = (mv); gm_n++; } while (0)
#endif

#ifdef USE_ASM_MOVEGEN
/* implementata in gen6502.s */
u8 asm_caps;
u8 __fastcall__ gen_moves6502(u16 *list);
#endif

#ifndef USE_ASM_MOVEGEN
static void add_promos(u16 *list, u8 from, u8 to)
{
    u16 b = (u16)(from | (to << 7));
    ADD_MOVE((u16)(b | (PROMO_Q << 14)));
    ADD_MOVE((u16)(b | (PROMO_R << 14)));
    ADD_MOVE((u16)(b | (PROMO_B << 14)));
    ADD_MOVE((u16)(b | (PROMO_N << 14)));
}
#endif

/* arrocco: generato a parte (serve attacked()), usato anche dal path asm */
static void add_castling(u16 *list)
{
    if (side == WHITE && kingSq[WHITE] == 4) {
        if ((castle & C_WK) && !board[5] && !board[6] &&
            !attacked(4, BLACK) && !attacked(5, BLACK))
            ADD_MOVE(MK_MOVE(4, 6, 0));
        if ((castle & C_WQ) && !board[1] && !board[2] && !board[3] &&
            !attacked(4, BLACK) && !attacked(3, BLACK))
            ADD_MOVE(MK_MOVE(4, 2, 0));
    } else if (side == BLACK && kingSq[BLACK] == 116) {
        if ((castle & C_BK) && !board[117] && !board[118] &&
            !attacked(116, WHITE) && !attacked(117, WHITE))
            ADD_MOVE(MK_MOVE(116, 118, 0));
        if ((castle & C_BQ) && !board[113] && !board[114] && !board[115] &&
            !attacked(116, WHITE) && !attacked(115, WHITE))
            ADD_MOVE(MK_MOVE(116, 114, 0));
    }
}

u8 gen_moves(u16 *list, u8 capsOnly)
{
#ifdef USE_ASM_MOVEGEN
    asm_caps = capsOnly;
    gm_n = gen_moves6502(list);
    if (!capsOnly) add_castling(list);
    return gm_n;
#else
    gm_n = 0;

    for (gm_rank = 0; gm_rank < 8; gm_rank++) {
        for (gm_file = 0; gm_file < 8; gm_file++) {
            gm_sq = (u8)((gm_rank << 4) | gm_file);
            gm_p = board[gm_sq];
            if (!gm_p) continue;
            gm_c = PC(gm_p);
            if (gm_c != side) continue;
            gm_t = PT(gm_p);

            if (gm_t == PAWN) {
                if (gm_c == WHITE) {
                    gm_to = (u8)(gm_sq + 16);
                    if (!(gm_to & 0x88) && !board[gm_to]) {
                        if (gm_rank == 6) add_promos(list, gm_sq, gm_to);
                        else {
                            ADD_MOVE(MK_MOVE(gm_sq, gm_to, 0));
                            if (gm_rank == 1) {
                                gm_to = (u8)(gm_sq + 32);
                                if (!board[gm_to]) ADD_MOVE(MK_MOVE(gm_sq, gm_to, 0));
                            }
                        }
                    }
                    gm_to = (u8)(gm_sq + 15);
                    if (!(gm_to & 0x88)) {
                        if (board[gm_to]) {
                            if (PC(board[gm_to]) != gm_c) {
                                if (gm_rank == 6) add_promos(list, gm_sq, gm_to);
                                else ADD_MOVE(MK_MOVE(gm_sq, gm_to, 0));
                            }
                        } else if (epSq >= 0 && gm_to == (u8)epSq) {
                            ADD_MOVE(MK_MOVE(gm_sq, gm_to, 0));
                        }
                    }
                    gm_to = (u8)(gm_sq + 17);
                    if (!(gm_to & 0x88)) {
                        if (board[gm_to]) {
                            if (PC(board[gm_to]) != gm_c) {
                                if (gm_rank == 6) add_promos(list, gm_sq, gm_to);
                                else ADD_MOVE(MK_MOVE(gm_sq, gm_to, 0));
                            }
                        } else if (epSq >= 0 && gm_to == (u8)epSq) {
                            ADD_MOVE(MK_MOVE(gm_sq, gm_to, 0));
                        }
                    }
                } else {
                    gm_to = (u8)(gm_sq - 16);
                    if (!(gm_to & 0x88) && !board[gm_to]) {
                        if (gm_rank == 1) add_promos(list, gm_sq, gm_to);
                        else {
                            ADD_MOVE(MK_MOVE(gm_sq, gm_to, 0));
                            if (gm_rank == 6) {
                                gm_to = (u8)(gm_sq - 32);
                                if (!board[gm_to]) ADD_MOVE(MK_MOVE(gm_sq, gm_to, 0));
                            }
                        }
                    }
                    gm_to = (u8)(gm_sq - 15);
                    if (!(gm_to & 0x88)) {
                        if (board[gm_to]) {
                            if (PC(board[gm_to]) != gm_c) {
                                if (gm_rank == 1) add_promos(list, gm_sq, gm_to);
                                else ADD_MOVE(MK_MOVE(gm_sq, gm_to, 0));
                            }
                        } else if (epSq >= 0 && gm_to == (u8)epSq) {
                            ADD_MOVE(MK_MOVE(gm_sq, gm_to, 0));
                        }
                    }
                    gm_to = (u8)(gm_sq - 17);
                    if (!(gm_to & 0x88)) {
                        if (board[gm_to]) {
                            if (PC(board[gm_to]) != gm_c) {
                                if (gm_rank == 1) add_promos(list, gm_sq, gm_to);
                                else ADD_MOVE(MK_MOVE(gm_sq, gm_to, 0));
                            }
                        } else if (epSq >= 0 && gm_to == (u8)epSq) {
                            ADD_MOVE(MK_MOVE(gm_sq, gm_to, 0));
                        }
                    }
                }
            } else if (gm_t == KNIGHT) {
#define JUMP(O) { \
                gm_to = (u8)(gm_sq + (O)); \
                if (!(gm_to & 0x88)) { \
                    gm_p = board[gm_to]; \
                    if ((!gm_p || PC(gm_p) != gm_c) && (!capsOnly || gm_p)) \
                        ADD_MOVE(MK_MOVE(gm_sq, gm_to, 0)); \
                } }
                JUMP(33) JUMP(31) JUMP(18) JUMP(14)
                JUMP(-33) JUMP(-31) JUMP(-18) JUMP(-14)
#undef JUMP
            } else if (gm_t == KING) {
#define STEP(O) { \
                gm_to = (u8)(gm_sq + (O)); \
                if (!(gm_to & 0x88)) { \
                    gm_p = board[gm_to]; \
                    if ((!gm_p || PC(gm_p) != gm_c) && (!capsOnly || gm_p)) \
                        ADD_MOVE(MK_MOVE(gm_sq, gm_to, 0)); \
                } }
                STEP(17) STEP(16) STEP(15) STEP(1)
                STEP(-1) STEP(-15) STEP(-16) STEP(-17)
#undef STEP
            } else {
#define SLIDE(O) \
                gm_to = (u8)(gm_sq + (O)); \
                while (!(gm_to & 0x88)) { \
                    gm_p = board[gm_to]; \
                    if (gm_p) { \
                        if (PC(gm_p) != gm_c) ADD_MOVE(MK_MOVE(gm_sq, gm_to, 0)); \
                        break; \
                    } \
                    if (!capsOnly) ADD_MOVE(MK_MOVE(gm_sq, gm_to, 0)); \
                    gm_to = (u8)(gm_to + (O)); \
                }
                if (gm_t == BISHOP) {
                    SLIDE(17) SLIDE(15) SLIDE(-15) SLIDE(-17)
                } else if (gm_t == ROOK) {
                    SLIDE(16) SLIDE(-16) SLIDE(1) SLIDE(-1)
                } else {
                    SLIDE(17) SLIDE(16) SLIDE(15) SLIDE(1)
                    SLIDE(-1) SLIDE(-15) SLIDE(-16) SLIDE(-17)
                }
#undef SLIDE
            }
        }
    }

    add_castling(list);
    return gm_n;
#endif
}

/* ------------------------------------------------------------------ */
/* make / unmake                                                       */
/* ------------------------------------------------------------------ */

static s16 pval(u8 p, u8 sq)
{
    s16 v;
    if (!p) return 0;
    v = (s16)(pieceVal[PT(p)] + pst_tab(PT(p))[pstIdx[PC(p)][sq]]);
    return (PC(p) == WHITE) ? v : (s16)(-v);
}

void refresh_eval(void)
{
    u8 rank, file, sq, p, t;

    evalWhite = 0;
    npm = 0;
    for (rank = 0; rank < 8; rank++)
        for (file = 0; file < 8; file++) {
            sq = (u8)((rank << 4) | file);
            p = board[sq];
            if (!p) continue;
            evalWhite = (s16)(evalWhite + pval(p, sq));
            t = PT(p);
            if (t != PAWN && t != KING) npm = (s16)(npm + pieceVal[t]);
        }
}

static u8 is_promo(u16 m)
{
    return (u8)(PT(board[M_FROM(m)]) == PAWN &&
                (M_TO(m) >= 112 || M_TO(m) < 16));
}

static void key_piece(u8 sq, u8 pc)
{
    if (pc) posKey ^= zPiece[pc][sq64[sq]];
}

static void make_move(u16 m)
{
    VLOCAL Undo *u;
    VLOCAL u8 from, to, pr, pc, cap, t, c, capSq, np, r;

    u = &undos[histPly];
    from = M_FROM(m); to = M_TO(m); pr = M_PROMO(m);
    pc = board[from];

    /* salva lo stato PRIMA di toccare la chiave */
    u->castle   = castle;
    u->ep       = epSq;
    u->halfmove = halfmove;
    u->moved    = pc;
    u->rookFrom = 0xFF;
    u->rookTo   = 0xFF;
    u->key      = posKey;
    u->eval     = evalWhite;
    u->npm      = npm;

    t = PT(pc);
    c = PC(pc);
    cap = board[to];
    capSq = to;

    if (t == PAWN && cap == 0 && epSq >= 0 && to == (u8)epSq) {
        /* en passant */
        capSq = (u8)((c == WHITE) ? (to - 16) : (to + 16));
        cap = board[capSq];
        board[capSq] = 0;
        key_piece(capSq, cap);
        evalWhite = (s16)(evalWhite - pval(cap, capSq));
        if (PT(cap) != PAWN && PT(cap) != KING) npm = (s16)(npm - pieceVal[PT(cap)]);
    } else if (cap) {
        key_piece(to, cap);
        evalWhite = (s16)(evalWhite - pval(cap, to));
        if (PT(cap) != PAWN && PT(cap) != KING) npm = (s16)(npm - pieceVal[PT(cap)]);
    }
    u->captured = cap;
    u->capSq = capSq;

    board[from] = 0;
    board[to] = pc;
    key_piece(from, pc);
    key_piece(to, pc);
    evalWhite = (s16)(evalWhite + pval(pc, to) - pval(pc, from));

    if (t == PAWN && (to >= 112 || to < 16)) {
        np = MAKE_PIECE(promoType[pr], c);
        board[to] = np;
        key_piece(to, pc);
        key_piece(to, np);
        evalWhite = (s16)(evalWhite + pval(np, to) - pval(pc, to));
        npm = (s16)(npm + pieceVal[promoType[pr]]);
    }
    if (t == PAWN || cap) halfmove = 0;

    if (t == KING) {
        kingSq[c] = to;
        if (to == (u8)(from + 2)) {
            u->rookFrom = (u8)(to + 1);
            u->rookTo = (u8)(to - 1);
        } else if (from >= 2 && to == (u8)(from - 2)) {
            u->rookFrom = (u8)(to - 2);
            u->rookTo = (u8)(to + 1);
        }
        if (u->rookFrom != 0xFF) {
            r = board[u->rookFrom];
            board[u->rookFrom] = 0;
            board[u->rookTo] = r;
            key_piece(u->rookFrom, r);
            key_piece(u->rookTo, r);
            evalWhite = (s16)(evalWhite + pval(r, u->rookTo) - pval(r, u->rookFrom));
        }
    }

    posKey ^= zCastle[castle];
    castle = (u8)(castle & castleMask[from] & castleMask[to]);
    posKey ^= zCastle[castle];

    if (epSq >= 0) posKey ^= zEp[epSq & 7];
    if (t == PAWN && to == (u8)(from + 32)) epSq = (s8)(from + 16);
    else if (t == PAWN && from >= 32 && to == (u8)(from - 32)) epSq = (s8)(from - 16);
    else epSq = -1;
    if (epSq >= 0) posKey ^= zEp[epSq & 7];

    side ^= 1;
    posKey ^= zSide;

    histPly++;
    keyHist[(u16)gamePly + (u16)histPly] = posKey;
}

static void unmake_move(u16 m)
{
    VLOCAL Undo *u;
    VLOCAL u8 from, to;

    from = M_FROM(m); to = M_TO(m);

    histPly--;
    u = &undos[histPly];
    side ^= 1;

    board[from] = u->moved;
    board[to] = 0;
    if (u->captured) board[u->capSq] = u->captured;
    if (u->rookFrom != 0xFF) {
        board[u->rookFrom] = board[u->rookTo];
        board[u->rookTo] = 0;
    }
    if (PT(u->moved) == KING) kingSq[PC(u->moved)] = from;

    castle = u->castle;
    epSq = u->ep;
    halfmove = u->halfmove;
    posKey = u->key;
    evalWhite = u->eval;
    npm = u->npm;
}

u8 push_move(u16 m)
{
    u8 me = side;

    make_move(m);
    if (attacked(kingSq[me], side)) {
        unmake_move(m);
        return 0;
    }
    return 1;
}

void pop_move(u16 m)
{
    unmake_move(m);
}

void commit_move(u16 m)
{
    u16 i;

    make_move(m);
    gamePly++;
    histPly = 0;
    /* La ricerca scrive keyHist[gamePly + histPly] fino a
       gamePly + (MAXSEARCH - 1): compatta PRIMA che gamePly possa
       far sforare il buffer (il vecchio limite MAXKEYS-4 era troppo
       alto e con gamePly=315 + 9 ply scriveva oltre keyHist[319]). */
    if (gamePly >= (u8)(MAXKEYS - MAXSEARCH - 1)) {
        /* compatta la storia: restano le ultime 64 posizioni */
        for (i = 0; i < 64; i++) keyHist[i] = keyHist[gamePly - 63 + i];
        gamePly = 63;
    }
}


/* ------------------------------------------------------------------ */
/* legalita' rapida: scacco e inchiodature calcolati una volta per nodo */
/* ------------------------------------------------------------------ */

static void compute_king_info(void)
{
    VLOCAL u8 s, p, t, k, d, first, us, them;
    VLOCAL s8 off;

    pinToken++;
    if (pinToken == 0) {          /* wrap: invalida tutto, capita ogni 256 nodi */
        u8 z;
        for (z = 0; z < 128; z++) pinStamp[z] = 0;
        pinToken = 1;
    }

    us = side;
    them = (u8)(side ^ 1);
    k = kingSq[us];
    inCheckFlag = attacked(k, them);

    for (d = 0; d < 8; d++) {
        off = qOff[d];
        first = 0xFF;
        s = (u8)(k + off);
        while (!(s & 0x88)) {
            p = board[s];
            if (p) {
                if (first == 0xFF) {
                    if (PC(p) != us) break;
                    first = s;
                } else {
                    if (PC(p) == them) {
                        t = PT(p);
                        if (orthoDir[d] ? (t == ROOK || t == QUEEN)
                                        : (t == BISHOP || t == QUEEN)) {
                            pinKind[first] = pinKindOfDir[d];
                            pinStamp[first] = pinToken;
                        }
                    }
                    break;
                }
            }
            s = (u8)(s + off);
        }
    }
}

/* versione lenta e sempre corretta: applica, verifica, annulla */
static u8 test_legal(u16 m)
{
    VLOCAL u8 me;

    me = side;

    make_move(m);
    if (attacked(kingSq[me], (u8)(me ^ 1))) {
        unmake_move(m);
        return 0;
    }
    unmake_move(m);
    return 1;
}

/* applica la mossa e la tiene: un solo make, usato quando serve il test
   completo (sotto scacco) o per l'en passant */
static u8 push_legal(u16 m)
{
    VLOCAL u8 me;

    me = side;

    make_move(m);
    if (attacked(kingSq[me], (u8)(me ^ 1))) {
        unmake_move(m);
        return 0;
    }
    return 1;
}

/* filtro veloce: usa scacco e inchiodature del nodo corrente */
static u8 is_legal(u16 m)
{
    VLOCAL u8 from, to, pc, kind;

#ifdef FORCE_SLOW_LEGAL
    return test_legal(m);          /* usato solo dai test differenziali */
#else
    if (inCheckFlag) return test_legal(m);

    from = M_FROM(m);
    to = M_TO(m);
    pc = board[from];

    if (PT(pc) == KING) {
        VLOCAL u8 ok;
        /* il re va tolto dalla scacchiera: puo' scoprire una linea */
        board[from] = 0;
        ok = (u8)(!attacked(to, (u8)(side ^ 1)));
        board[from] = pc;
        return ok;
    }

    kind = (pinStamp[from] == pinToken) ? pinKind[from] : 0;
    if (kind) {
        switch (kind) {
        case 1:  return (u8)((from & 7) == (to & 7));
        case 2:  return (u8)((from >> 4) == (to >> 4));
        case 3:  return (u8)(((from >> 4) - (from & 7)) == ((to >> 4) - (to & 7)));
        default: return (u8)(((from >> 4) + (from & 7)) == ((to >> 4) + (to & 7)));
        }
    }

    /* l'en passant puo' scoprire il re anche senza pedone inchiodato */
    if (PT(pc) == PAWN && epSq >= 0 && to == (u8)epSq) return test_legal(m);
    return 1;
#endif
}

u8 legal_moves(u16 *list)
{
    u8 n, i, k = 0;

    n = gen_moves(list, 0);
    compute_king_info();
    for (i = 0; i < n; i++)
        if (is_legal(list[i])) {
            list[k] = list[i];
            k++;
        }
    return k;
}

u8 has_legal_moves(void)
{
    static u16 buf[MAXMOVES];
    return (u8)(legal_moves(buf) != 0);
}

/* ------------------------------------------------------------------ */
/* valutazione                                                         */
/* ------------------------------------------------------------------ */

s16 evaluate(void)
{
    VLOCAL s16 v;
    VLOCAL u8 iw, ib;

    v = evalWhite;

    if (npm <= 1300) {
        iw = pstIdx[WHITE][kingSq[WHITE]];
        ib = pstIdx[BLACK][kingSq[BLACK]];
        v = (s16)(v + pstKingEnd[iw] - pstKing[iw]);
        v = (s16)(v - (pstKingEnd[ib] - pstKing[ib]));
    }
    return (side == WHITE) ? v : (s16)(-v);
}

/* ------------------------------------------------------------------ */
/* regole di patta                                                     */
/* ------------------------------------------------------------------ */

u8 repetition_count(void)
{
    u8 i, cnt = 0;
    u16 base = (u16)((u16)gamePly + (u16)histPly);
    u16 lim = halfmove;

    /* la chiave contiene il lato al tratto: bastano le posizioni a
       distanza pari (meta' del lavoro), e sotto le 8 semimosse una
       ripetizione e' impossibile */
    if (halfmove < 8) return 0;
    if (lim > base) lim = base;
    for (i = 2; i <= lim; i += 2)
        if (keyHist[base - i] == keyHist[base]) cnt++;
    return cnt;
}

u8 fifty_moves(void)
{
    return (u8)(halfmove >= 100);
}

u8 insufficient_material(void)
{
    u8 rank, file, sq, p, t, c;
    u8 nw = 0, nb = 0, bw = 0, bb = 0, bwcol = 0, bbcol = 0;

    for (rank = 0; rank < 8; rank++)
        for (file = 0; file < 8; file++) {
            sq = (u8)((rank << 4) | file);
            p = board[sq];
            if (!p) continue;
            t = PT(p);
            c = PC(p);
            if (t == PAWN || t == ROOK || t == QUEEN) return 0;
            if (t == KNIGHT) {
                if (c == WHITE) nw++; else nb++;
            } else if (t == BISHOP) {
                if (c == WHITE) { bw++; bwcol = (u8)(((rank + file) & 1)); }
                else            { bb++; bbcol = (u8)(((rank + file) & 1)); }
            }
        }

    if (nw + nb + bw + bb == 0) return 1;      /* K vs K */
    if (nw + nb + bw + bb == 1) return 1;      /* K+pezzo minore vs K */
    if (nw == 0 && nb == 0 && bw == 1 && bb == 1 && bwcol == bbcol)
        return 1;                              /* alfieri dello stesso colore */
    if (nw == 2 && nb == 0 && bw == 0 && bb == 0) return 1;  /* KNN vs K */
    return 0;
}

/* ------------------------------------------------------------------ */
/* ricerca                                                             */
/* ------------------------------------------------------------------ */

u8 quiesceMax = 4;      /* profondita' massima della quiescenza */
u8 thinkSeconds = 0;    /* 0 = nessun limite di tempo */
u8 thinkDepth;          /* ultima profondita' completata */

static u16 killer[MAXPLY][2];   /* mosse che hanno causato un taglio */

static u32 nodeLimit;
static u32 deadlineTicks;
static u8  stopFlag;
static u16 prevBest;

static void time_check(void)
{
    if (deadlineTicks && (nodes & 0x0F) == 0 && (u32)clock() >= deadlineTicks)
        stopFlag = 1;
}

static s16 move_score(u16 m)
{
    VLOCAL u8 from, to, pc, cap;
    VLOCAL s16 s;

    from = M_FROM(m); to = M_TO(m);
    pc = board[from]; cap = board[to];
    s = 0;

    if (cap) s = (s16)(10 * pieceVal[PT(cap)] - pieceVal[PT(pc)]);
    else if (PT(pc) == PAWN && epSq >= 0 && to == (u8)epSq)
        s = (s16)(10 * pieceVal[PAWN] - pieceVal[PAWN]);
    if (PT(pc) == PAWN && (to >= 112 || to < 16))
        s = (s16)(s + 800 + pieceVal[promoType[M_PROMO(m)]]);
    return s;
}

static void pick_swap(u16 *list, s16 *sc, u8 i, u8 bj)
{
    VLOCAL u16 tm;
    VLOCAL s16 ts;
    tm = list[i]; list[i] = list[bj]; list[bj] = tm;
    ts = sc[i];   sc[i] = sc[bj];     sc[bj] = ts;
}

static u8 is_tactical(u16 m)
{
    VLOCAL u8 to, pc;

    to = M_TO(m);
    pc = board[M_FROM(m)];

    if (board[to]) return 1;
    if (PT(pc) == PAWN) {
        if (to >= 112 || to < 16) return 1;              /* promozione */
        if (epSq >= 0 && to == (u8)epSq) return 1;       /* en passant */
    }
    return 0;
}

/* porta davanti le mosse tattiche: cosi' l'ordinamento alpha-beta
   costa O(n) invece di O(n^2) su tutte le mosse */
static u8 partition_tactical(u16 *list, u8 n)
{
    VLOCAL u8 i, k;
    VLOCAL u16 t;

    k = 0;

    for (i = 0; i < n; i++)
        if (is_tactical(list[i])) {
            t = list[i]; list[i] = list[k]; list[k] = t;
            k++;
        }
    return k;
}

/* sposta mv in posizione pos (se presente in [pos,n)) */
static u8 promote_move(u16 *list, u8 n, u8 pos, u16 mv)
{
    VLOCAL u8 j;
    VLOCAL u16 t;

    for (j = pos; j < n; j++)
        if (list[j] == mv) {
            t = list[j]; list[j] = list[pos]; list[pos] = t;
            return (u8)(pos + 1);
        }
    return pos;
}

static u16 pick(u16 *list, s16 *sc, u8 n, u8 i)
{
    VLOCAL u8 j, bj;
    VLOCAL s16 bs, v;

    bj = i; bs = -30000;

    for (j = i; j < n; j++) {
        v = sc[j];
        if (v > bs) { bs = v; bj = j; }
    }
    pick_swap(list, sc, i, bj);
    return list[i];
}

static u16 pick_root(u16 *list, s16 *sc, u8 n, u8 i, u16 pref)
{
    static u8 j, bj;
    static s16 bs, v;

    bj = i; bs = -30000;

    for (j = i; j < n; j++) {
        v = sc[j];
        if (list[j] == pref) v = (s16)(v + 20000);
        if (v > bs) { bs = v; bj = j; }
    }
    pick_swap(list, sc, i, bj);
    return list[i];
}

static s16 quiesce(s16 alpha, s16 beta, u8 ply, u8 qd)
{
    u16 *list;
    s16 *sc;
    u8 n, i, from, to, pc, chk, legal = 0, svTok;
    u16 m;
    s16 stand = 0, score, best;

    nodes++;
    if (nodes >= nodeLimit) { stopFlag = 1; return alpha; }
    time_check();
    if (stopFlag) return alpha;
    if (ply >= (u8)(MAXPLY - 1) || qd >= quiesceMax) return evaluate();

    list = moveBuf[ply];
    sc = scoreBuf[ply];
    n = gen_moves(list, 1);
    compute_king_info();
    chk = inCheckFlag;
    svTok = pinToken;

    if (chk) {
        /* sotto scacco: niente stand-pat, si cercano tutte le evasioni */
        n = gen_moves(list, 0);
        best = -EVAL_INF;
    } else {
        stand = evaluate();
        if (stand >= beta) return beta;
        if (stand > alpha) alpha = stand;
        best = alpha;
    }

    for (i = 0; i < n; i++) sc[i] = move_score(list[i]);
    for (i = 0; i < n; i++) {
        m = pick(list, sc, n, i);
        if (!chk) {
            from = M_FROM(m);
            to = M_TO(m);
            pc = board[from];
            if (PT(pc) != PAWN && board[to] &&
                (s16)(stand + pieceVal[PT(board[to])] + 200) <= alpha)
                continue;
        }
        if (chk) {
            if (!push_legal(m)) continue;
        } else {
            if (!is_legal(m)) continue;
            make_move(m);
        }
        legal = 1;
        score = (s16)(-quiesce((s16)-beta, (s16)-alpha, (u8)(ply + 1), (u8)(qd + 1)));
        unmake_move(m);
        pinToken = svTok;           /* invalida le inchiodature del figlio */
        inCheckFlag = chk;
        if (stopFlag) return alpha;
        if (score > best) best = score;
        if (best > alpha) alpha = best;
        if (alpha >= beta) return beta;
    }
    if (chk && !legal) return (s16)(-(EVAL_MATE - (s16)ply));
    return best;
}

static s16 search(s8 depth, s16 alpha, s16 beta, u8 ply)
{
    u16 *list;
    s16 *sc;
    u8 n, i, k, chk, legal = 0, svTok;
    u16 m;
    s16 score, best = -EVAL_INF;

    nodes++;
    if (nodes >= nodeLimit) { stopFlag = 1; return 0; }
    time_check();
    if (stopFlag) return 0;
    if (ply >= (u8)(MAXPLY - 1)) return evaluate();
    if (halfmove >= 100) return 0;
    if (halfmove >= 8 && repetition_count()) return 0;
    if (depth <= 0) return quiesce(alpha, beta, ply, 0);

    list = moveBuf[ply];
    sc = scoreBuf[ply];
    n = gen_moves(list, 0);
    compute_king_info();
    chk = inCheckFlag;
    svTok = pinToken;

    k = partition_tactical(list, n);
    for (i = 0; i < k; i++) sc[i] = move_score(list[i]);
    i = k;
    if (killer[ply][0]) i = promote_move(list, n, i, killer[ply][0]);
    if (killer[ply][1]) i = promote_move(list, n, i, killer[ply][1]);
    for (i = 0; i < n; i++) {
        m = (i < k) ? pick(list, sc, k, i) : list[i];
        if (chk) {
            if (!push_legal(m)) continue;
        } else {
            if (!is_legal(m)) continue;
            make_move(m);
        }
        legal = 1;
        score = (s16)(-search((s8)(depth - 1), (s16)-beta, (s16)-alpha, (u8)(ply + 1)));
        unmake_move(m);
        pinToken = svTok;
        inCheckFlag = chk;
        if (stopFlag) return 0;
        if (score > best) best = score;
        if (best > alpha) alpha = best;
        if (alpha >= beta) {
            if (!is_tactical(m)) {          /* killer: solo mosse tranquille */
                killer[ply][1] = killer[ply][0];
                killer[ply][0] = m;
            }
            break;
        }
    }
    if (!legal) return chk ? (s16)(-(EVAL_MATE - (s16)ply)) : 0;
    return best;
}

u16 think(u8 maxDepth, u32 maxNodes, s16 *score)
{
    static u16 rootList[MAXMOVES];
    static s16 rootSc[MAXMOVES];
    u8 n, i, d;
    u16 m, bestMove;
    s16 alpha, best, s;

    nodes = 0;
    thinkDepth = 0;
    stopFlag = 0;
    for (i = 0; i < MAXPLY; i++) {
        killer[i][0] = 0;
        killer[i][1] = 0;
    }
    nodeLimit = (maxNodes == 0) ? 0xFFFFFFFFUL : maxNodes;
    deadlineTicks = 0;
    if (thinkSeconds)
        deadlineTicks = (u32)clock() + (u32)thinkSeconds * (u32)CLOCKS_PER_SEC;
    histPly = 0;
    prevBest = 0;

    n = legal_moves(rootList);
    if (n == 0) { *score = 0; return MOVE_NONE; }

    for (i = 0; i < n; i++) rootSc[i] = move_score(rootList[i]);
    bestMove = rootList[0];
    *score = 0;

    for (d = 1; d <= maxDepth; d++) {
        alpha = -EVAL_INF;
        best = -EVAL_INF;
        for (i = 0; i < n; i++) {
            m = pick_root(rootList, rootSc, n, i, prevBest);
            if (!push_move(m)) continue;
            s = (s16)(-search((s8)(d - 1), (s16)-EVAL_INF, (s16)-alpha, 1));
            pop_move(m);
            if (stopFlag) break;
            if (s > best) { best = s; bestMove = m; }
            if (best > alpha) alpha = best;
        }
        if (stopFlag) break;
        *score = best;
        thinkDepth = d;
        prevBest = bestMove;
        if (best >= EVAL_MATE - 200 || best <= -(EVAL_MATE - 200)) break;
        if (maxNodes && nodes >= maxNodes) break;
    }

    histPly = 0;
    return bestMove;
}

/* ------------------------------------------------------------------ */
/* parsing mosse                                                       */
/* ------------------------------------------------------------------ */

static char lowerc(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return (char)(ch - 'A' + 'a');
    return ch;
}

u16 parse_move(const char *s)
{
    static u16 list[MAXMOVES];
    char f1, r1, f2, r2;
    u8 from, to, promo = PROMO_Q;
    u8 n, i;

    while (*s == ' ' || *s == '\t') s++;
    f1 = lowerc(*s);
    if (f1 < 'a' || f1 > 'h') return MOVE_NONE;
    s++;
    r1 = *s;
    if (r1 < '1' || r1 > '8') return MOVE_NONE;
    s++;
    if (*s == '-' || *s == 'x' || *s == ' ' || *s == ':') s++;
    f2 = lowerc(*s);
    if (f2 < 'a' || f2 > 'h') return MOVE_NONE;
    s++;
    r2 = *s;
    if (r2 < '1' || r2 > '8') return MOVE_NONE;
    s++;
    switch (lowerc(*s)) {
        case 'n': promo = PROMO_N; break;
        case 'b': promo = PROMO_B; break;
        case 'r': promo = PROMO_R; break;
        case 'q': promo = PROMO_Q; break;
        default: break;
    }

    from = (u8)(((r1 - '1') << 4) | (f1 - 'a'));
    to   = (u8)(((r2 - '1') << 4) | (f2 - 'a'));

    n = legal_moves(list);
    for (i = 0; i < n; i++) {
        if (M_FROM(list[i]) != from || M_TO(list[i]) != to) continue;
        if (is_promo(list[i])) {
            if (M_PROMO(list[i]) == promo) return list[i];
        } else {
            return list[i];
        }
    }
    return MOVE_NONE;
}
