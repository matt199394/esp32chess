/*
 * san_tool.h - utilita' SAN condivise dai generatori (pgn2moves.c,
 * endgames2h.c): convertono e ricontrollano la notazione algebrica
 * usando legal_moves() del motore vero. Richiede chess.h incluso prima.
 */

#ifndef SAN_TOOL_H
#define SAN_TOOL_H

#include <string.h>
#include <ctype.h>

static u16 sanList[MAXMOVES];

/* ---------------- ricalcolo SAN standard (per verifica) ------------- */

/* SAN standard della mossa m, calcolata sulla posizione corrente del
 * motore (PRIMA di applicarla) e SENZA suffissi +/# */
static void san_of_move(u16 m, char *buf)
{
    u8 from = M_FROM(m), to = M_TO(m), pr = M_PROMO(m);
    u8 p = board[from], pt = PT(p), pc = PC(p);
    u8 others[16], no = 0;
    u8 i, n = legal_moves(sanList);
    int pos = 0;
    int isCastle = (pt == KING && ((u8)(to - from) == 2 || (u8)(from - to) == 2));
    int isEp = (pt == PAWN && to == (u8)epSq && (to & 7) != (from & 7));
    int capture = (board[to] != EMPTY) || isEp;

    if (isCastle) {
        strcpy(buf, (to > from) ? "O-O" : "O-O-O");
        return;
    }

    if (pt != PAWN) {
        for (i = 0; i < n; i++) {
            u8 f = M_FROM(sanList[i]);
            if (M_TO(sanList[i]) == to && f != from &&
                PT(board[f]) == pt && PC(board[f]) == pc)
                others[no++] = f;
        }
        buf[pos++] = " PNBRQK"[pt];
        if (no) {
            int fSame = 0, rSame = 0;
            u8 k;
            for (k = 0; k < no; k++) {
                if ((others[k] & 7) == (from & 7)) fSame = 1;
                if ((others[k] >> 4) == (from >> 4)) rSame = 1;
            }
            if (!fSame)      buf[pos++] = (char)('a' + (from & 7));
            else if (!rSame) buf[pos++] = (char)('1' + (from >> 4));
            else {
                buf[pos++] = (char)('a' + (from & 7));
                buf[pos++] = (char)('1' + (from >> 4));
            }
        }
        if (capture) buf[pos++] = 'x';
    } else {
        if (capture) buf[pos++] = (char)('a' + (from & 7)), buf[pos++] = 'x';
    }

    buf[pos++] = (char)('a' + (to & 7));
    buf[pos++] = (char)('1' + (to >> 4));
    if (pt == PAWN && ((to >> 4) == 0 || (to >> 4) == 7)) {
        buf[pos++] = '=';
        buf[pos++] = "NBRQ"[pr];
    }
    buf[pos] = 0;
}

/* ---------------- parse di un token SAN ---------------------------- */

static char *norm(const char *t, char *out)
{
    int i = 0;
    while (*t) {
        if (*t == '!' || *t == '?' || *t == '+' || *t == '#') break;
        out[i++] = *t++;
    }
    out[i] = 0;
    return out;
}

/* cerca tra le mosse legali quella che corrisponde al token SAN;
 * ritorna la mossa o MOVE_NONE */
static u16 match_san(const char *tok)
{
    char s[16];
    const char *p;
    u8 piece = PAWN, dfile = 0xFF, drank = 0xFF, tfile, trank, promo = 0;
    int len;
    u8 n, k;
    u16 found = MOVE_NONE;
    int ncand = 0;

    norm(tok, s);
    len = (int)strlen(s);
    (void)len;

    if (strcmp(s, "O-O") == 0 || strcmp(s, "0-0") == 0) {
        u8 kf = (side == WHITE) ? 4 : 0x74;
        return MK_MOVE(kf, (u8)(kf + 2), 0);
    }
    if (strcmp(s, "O-O-O") == 0 || strcmp(s, "0-0-0") == 0) {
        u8 kf = (side == WHITE) ? 4 : 0x74;
        return MK_MOVE(kf, (u8)(kf - 2), 0);
    }

    if (s[0] == 'K') { piece = KING; }
    else if (s[0] == 'Q') { piece = QUEEN; }
    else if (s[0] == 'R') { piece = ROOK; }
    else if (s[0] == 'B') { piece = BISHOP; }
    else if (s[0] == 'N') { piece = KNIGHT; }

    /* promo "=Q" */
    for (p = s; *p; p++)
        if (*p == '=') {
            char c = (char)toupper((unsigned char)p[1]);
            promo = (c == 'N') ? PROMO_N : (c == 'B') ? PROMO_B :
                    (c == 'R') ? PROMO_R : PROMO_Q;
        }

    /* casa di destinazione: ultime due lettere alfanumeriche prima di '=' */
    {
        int end = len;
        for (p = s; *p; p++) if (*p == '=') { end = (int)(p - s); break; }
        while (end > 0 && !isalnum((unsigned char)s[end - 1])) end--;
        trank = (u8)(s[end - 1] - '1');
        tfile = (u8)(s[end - 2] - 'a');
        end -= 2;
        /* cattura e disambiguazioni nella parte restante */
        p = s + (piece != PAWN ? 1 : 0);
        while (p < s + end) {
            if (*p == 'x') { p++; continue; }
            if (*p >= 'a' && *p <= 'h') dfile = (u8)(*p - 'a');
            else if (*p >= '1' && *p <= '8') drank = (u8)(*p - '1');
            p++;
        }
    }

    n = legal_moves(sanList);
    for (k = 0; k < n; k++) {
        u16 m = sanList[k];
        u8 f = M_FROM(m);
        if (M_TO(m) != (u8)((trank << 4) | tfile)) continue;
        if (PT(board[f]) != piece) continue;
        if (dfile != 0xFF && (f & 7) != dfile) continue;
        if (drank != 0xFF && (f >> 4) != drank) continue;
        if (piece == PAWN && (trank == 0 || trank == 7)) {
            if (M_PROMO(m) != promo) continue;
        } else if (M_PROMO(m) != 0) continue;
        found = m;
        ncand++;
    }
    if (ncand != 1) return MOVE_NONE;
    return found;
}

#endif /* SAN_TOOL_H */
