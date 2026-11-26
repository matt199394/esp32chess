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

/* ---- risposte ESP32 -> C64 ---- */
#define RSP_ILLEGAL   0x00
#define RSP_ACK       0x01
#define RSP_BEST_MOVE 0x02   /* + mossa lo, mossa hi, score lo, score hi, depth, flags */
#define RSP_GAME_OVER 0x03   /* + reason, winner */
#define RSP_HIST      0x04   /* + n lo, n hi, poi n mosse (2 byte ciascuna) */
#define RSP_MOVED     0x05   /* mossa dell'umano accettata: + flags (u8) */

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

/* ---- flags di BEST_MOVE ---- */
#define F_CHECK    0x01
#define F_GAMEOVER 0x02

/* ---- livelli ---- */
#define LEVEL_EASY   0
#define LEVEL_MEDIUM 1
#define LEVEL_HARD   2
#define LEVEL_EXPERT 3

#endif /* PROTOCOL_H */
