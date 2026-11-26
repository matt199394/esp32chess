/*
 * chess.h - motore scacchi portabile
 *
 * Obiettivi di progetto:
 *   - C89, nessun float, nessuna allocazione dinamica, nessun I/O
 *   - tipi a larghezza fissa (u8/s8/u16/s16/u32) compatibili con cc65
 *   - board 0x88 (128 byte), mosse impacchettate in 16 bit
 *   - array di lavoro statici (niente array automatici grandi:
 *     su C64 lo stack software e' piccolo)
 *
 * Codifica pezzi: 0 = vuoto, tipo 1..6 (P N B R Q K), colore nel bit 3
 * (bianco 1..6, nero 9..14).
 */

#ifndef CHESS_H
#define CHESS_H

#if defined(__CC65__)
typedef unsigned char  u8;
typedef signed char    s8;
typedef unsigned int   u16;   /* cc65: int e' 16 bit */
typedef int            s16;
typedef unsigned long  u32;
#else
typedef unsigned char  u8;
typedef signed char    s8;
typedef unsigned short u16;
typedef short          s16;
typedef unsigned long  u32;
#endif

/* tipi di pezzo */
#define EMPTY  0
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6

#define WHITE 0
#define BLACK 1

/* diritti di arrocco */
#define C_WK 1
#define C_WQ 2
#define C_BK 4
#define C_BQ 8

/* Su cc65 le variabili locali finiscono sullo stack software e ogni
   accesso diventa un (sp),y: nelle funzioni calde conviene tenerle in
   BSS. Su PC il compilatore le tiene comunque in registro. */
#if defined(__CC65__)
#define VLOCAL static
#else
#define VLOCAL
#endif

#define MAKE_PIECE(t,c) ((u8)((t) | ((c) << 3)))
#define PT(p)           ((u8)((p) & 7))
#define PC(p)           ((u8)((p) >> 3))

/* mossa: from 7 bit | to 7 bit | promozione 2 bit */
#define MK_MOVE(f,t,pr) ((u16)(((f) | ((t) << 7)) | ((pr) << 14)))
#define M_FROM(m)       ((u8)((m) & 0x7F))
#define M_TO(m)         ((u8)(((m) >> 7) & 0x7F))
#define M_PROMO(m)      ((u8)(((m) >> 14) & 3))
#define MOVE_NONE       ((u16)0xFFFF)

#define PROMO_N 0
#define PROMO_B 1
#define PROMO_R 2
#define PROMO_Q 3

/*
 * Dimensioni: su C64 servono ~5 KB di RAM per le liste di mosse.
 * MAXMOVES 120 basta per qualunque posizione raggiungibile in partita
 * (il massimo teorico con 9 donne e' 218, ma non si verifica mai).
 */
#if defined(__CC65__)
#define MAXPLY    10
#define MAXMOVES  120
#define MAXSEARCH 10
#else
#define MAXPLY    16
#define MAXMOVES  256
#define MAXSEARCH 16
#endif

#define MAXKEYS 320

/* stato della posizione (accessibile: serve a FEN/perft) */
extern u8  board[128];
extern u8  side;          /* 0 = bianco, 1 = nero */
extern u8  castle;
extern s8  epSq;          /* -1 oppure casa 0x88 */
extern u8  halfmove;      /* semimosse dall'ultima irrversibile */
extern u8  kingSq[2];
extern u16 posKey;
extern u8  gamePly;       /* indice in keyHist della posizione corrente */
extern u16 keyHist[MAXKEYS];
extern u32 nodes;         /* nodi visitati dall'ultima think() */
extern u8  quiesceMax;    /* profondita' massima della quiescenza (0 = off) */
extern u8  thinkSeconds;  /* limite di tempo per think() (0 = nessuno) */
extern u8  thinkDepth;    /* ultima profondita' completata da think() */

/* valutazione (centipedoni) usata dalla ricerca */
#define EVAL_INF  30000
#define EVAL_MATE 20000

void init_engine(void);      /* da chiamare una volta: tabelle zobrist/PST */
void new_game(void);
void reset_history(void);
void compute_key(void);
void refresh_eval(void);

u8   gen_moves(u16 *list, u8 capsOnly);   /* pseudo-legali */
u8   push_move(u16 m);                    /* applica se legale, 1 = ok */
void pop_move(u16 m);                     /* annulla push_move */
void commit_move(u16 m);                  /* push_move + avanza la partita */
u8   legal_moves(u16 *list);
u8   has_legal_moves(void);
u8   in_check(void);

s16  evaluate(void);                      /* dal punto di vista di chi muove */
u16  think(u8 maxDepth, u32 maxNodes, s16 *score);

u8   repetition_count(void);   /* 0 = nuova, 1 = 2a volta, 2 = 3a volta */
u8   fifty_moves(void);
u8   insufficient_material(void);

u16  parse_move(const char *s);   /* "e2e4", "e2-e4", "e7e8q" -> mossa o MOVE_NONE */

#endif /* CHESS_H */
