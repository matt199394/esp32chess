/* protocol.h - costanti del protocollo ESP32 <-> C64 (bozza v0)
 * KEEP IN SYNC con c64/protocol.h e docs/PROTOCOL.md
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

/* ---- comandi C64 -> ESP32 ---- */
#define CMD_NEW_GAME  0x4E   /* 'N' */
#define CMD_SET_COLOR 0x43   /* 'C' + 1 byte: 0 bianco, 1 nero */
#define CMD_SET_LEVEL 0x4C   /* 'L' + 1 byte: 0..3 */
#define CMD_MOVE      0x4D   /* 'M' + 2 byte: mossa lo, hi */
#define CMD_UNDO      0x55   /* 'U' */
#define CMD_QUIT      0x51   /* 'Q' */
#define CMD_GET_HIST  0x47   /* 'G' - risponde con lo storico mosse (resync) */
#define CMD_GO        0x45   /* 'E' - muovi se e' il turno del computer */
#define CMD_FAME_LOAD 0x46   /* 'F' + 1 byte: indice partita famosa (0..29);
                             * risponde ACK + n lo + n hi (semimosse) */
#define CMD_FAME_NEXT 0x50   /* 'P' - applica la prossima semimossa della
                             * partita famosa: RSP_MOVED + flags + m lo + m hi,
                             * oppure RSP_GAME_OVER a lista esaurita (reason 0:
                             * risultato storico, non un matto) */
#define CMD_END_LOAD  0x53   /* 'S' + 1 byte: indice finale (0..END_COUNT-1);
                             * risponde ACK e carica la posizione (l'umano
                             * gioca il lato che vince); se dopo il carico
                             * tocca al motore la UI manda CMD_GO */

/* ---- risposte ESP32 -> C64 ---- */
#define RSP_ILLEGAL   0x00
#define RSP_ACK       0x01
#define RSP_BEST_MOVE 0x02   /* + mossa lo, mossa hi, score lo, score hi, depth, flags */
#define RSP_GAME_OVER 0x03   /* + reason, winner */
#define RSP_HIST      0x04   /* + n lo, n hi, poi n mosse (2 byte ciascuna) */
#define RSP_MOVED     0x05   /* mossa accettata (umano o replay famosa):
                             * + flags (u8) e, nel replay, + m lo + m hi */
                               /*   flags: F_CHECK | F_DRAW50 | F_DRAWREP */
#define OVER_LIST_END 0      /* GAME_OVER: lista semimosse esaurita (famose) */

/* ---- reason di GAME_OVER ---- */
#define OVER_MATE       1
#define OVER_STALEMATE  2
#define OVER_50MOVE     3
#define OVER_REPETITION 4
#define OVER_MATERIAL   5

/* ---- winner di GAME_OVER ---- */
#define WINNER_WHITE 0
#define WINNER_BLACK 1
#define WINNER_DRAW  0xFF

/* ---- flags di BEST_MOVE e RSP_MOVED ---- */
#define F_CHECK    0x01
#define F_GAMEOVER 0x02
#define F_DRAW50   0x04   /* 50 mosse: patta reclamabile, la partita continua */
#define F_DRAWREP  0x08   /* posizione ripetuta: patta reclamabile, continua */

/* ---- livelli ---- */
#define LEVEL_EASY   0
#define LEVEL_MEDIUM 1
#define LEVEL_HARD   2
#define LEVEL_EXPERT 3

#endif /* PROTOCOL_H */
