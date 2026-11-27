/* endgames.h - finali: posizioni iniziali e metadati.
 * GENERATO da tools/endgames2h.c: NON modificare a mano.
 * Board a8..h1 (64 byte), codifica pezzi del motore (chess.h).
 * Posizioni e soluzioni validate con legal_moves()/in_check().
 */
#ifndef ENDGAMES_H
#define ENDGAMES_H

#include "chess.h"

#define END_COUNT 7

static const u8 endBoard[END_COUNT][64] = {
    /* RE E DONNA MATTO IN 1 */
    {0,0,0,0,0,0,0,14,0,0,0,0,0,0,0,0,0,0,0,0,0,6,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* RE E DONNA MATTO IN 2 */
    {0,0,0,0,0,0,0,14,0,0,0,0,0,0,0,0,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,5,0,0,0,0,0,0,0},
    /* RE E TORRE MATTO IN 1 */
    {0,0,0,14,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,0,0,0,0,0,0,0},
    /* RE E TORRE MUOVE NERO */
    {0,0,0,0,0,0,14,0,0,0,0,0,0,0,0,0,0,0,0,0,0,6,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* DUE TORRI MATTO IN 2 */
    {0,0,0,0,0,0,14,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,0,0,0,0,0,0,0,0,4,0,0,0,0,6,0},
    /* CAVALLO E ALFIERE MATTO 1 */
    {14,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* LUCENA VITTORIA TECNICA */
    {0,0,0,0,6,0,0,0,0,0,14,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,12,0,0,0,0,0,0,0,0,4,0},
};

/* lato di tratto alla posizione iniziale */
static const u8 endSide[END_COUNT] = { 0, 0, 0, 1, 0, 0, 0 };

/* diritti di arrocco */
static const u8 endCastle[END_COUNT] = { 0, 0, 0, 0, 0, 0, 0 };

/* casa en passant (0xFF = nessuna) */
static const u8 endEp[END_COUNT] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* il lato che gioca l'umano (quello che vince) */
static const u8 endWinner[END_COUNT] = { 0, 0, 0, 0, 0, 0, 0 };

/* 0 = tecnica, N = matto in N mosse dell'umano */
static const u8 endTarget[END_COUNT] = { 1, 2, 1, 1, 2, 1, 0 };

#endif /* ENDGAMES_H */
