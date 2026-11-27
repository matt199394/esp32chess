/* endgames.h - finali: etichette e board per la UI C64.
 * GENERATO da tools/endgames2h.c: NON modificare a mano.
 * Board a8..h1 (64 byte), codifica pezzi (t | colore<<3) come in
 * chessui.c. Deve restare allineato con esp32/endgames.h.
 */
#ifndef C64_ENDGAMES_H
#define C64_ENDGAMES_H

#define END_COUNT 7

static const char endLabel[END_COUNT][32] = {
    "RE E DONNA MATTO IN 1",
    "RE E DONNA MATTO IN 2",
    "RE E TORRE MATTO IN 1",
    "RE E TORRE MUOVE NERO",
    "DUE TORRI MATTO IN 2",
    "CAVALLO E ALFIERE MATTO 1",
    "LUCENA VITTORIA TECNICA",
};

static const unsigned char endBoard[END_COUNT][64] = {
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
static const unsigned char endSide[END_COUNT] = { 0, 0, 0, 1, 0, 0, 0 };

/* il lato che gioca l'umano (quello che vince) */
static const unsigned char endWinner[END_COUNT] = { 0, 0, 0, 0, 0, 0, 0 };

#endif /* C64_ENDGAMES_H */
