/*
 * pgn2moves.c - converte e VALIDA le partite famose (SAN) nel formato
 * u16 del motore e genera esp32/famous_games.h.
 *
 * Ogni mossa SAN viene confrontata con legal_moves() del motore vero:
 * se una partita non si riproduce legalmente dalla posizione iniziale,
 * il tool abortisce (fonte sbagliata => da correggere).
 * Ricontrolla anche ricalcolando la SAN standard (disambiguazioni,
 * catture, promozioni) e segnala le differenze.
 *
 * Build (PC):  gcc -I esp32 tools/pgn2moves.c esp32/chess.c -o /tmp/pgn2moves
 * Run:         /tmp/pgn2moves /tmp/all_games.txt > esp32/famous_games.h
 *
 * Formato input: nome (riga), risultato (1-0 / 0-1 / 1/2-1/2), SAN (riga),
 * riga vuota; una partita per blocco.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "chess.h"

static u16 list[MAXMOVES];

/* ---------------- ricalcolo SAN standard (per verifica) ------------- */

static int slide_reaches(const u8 *b, u8 from, u8 to, int diag, int straight)
{
    int df = (int)(to & 7) - (int)(from & 7);
    int dr = (int)(to >> 4) - (int)(from >> 4);
    int sf, sr;

    if (diag && dr != 0 && dr == df)            sf = (df > 0) - (df < 0), sr = 1;
    else if (diag && dr != 0 && dr == -df)      sf = (df > 0) - (df < 0), sr = -1;
    else if (straight && dr == 0 && df != 0)    sf = (df > 0) - (df < 0), sr = 0;
    else if (straight && df == 0 && dr != 0)    sf = 0, sr = (dr > 0) - (dr < 0);
    else return 0;

    {
        int f = (int)(from & 7) + sf;
        int r = (int)(from >> 4) + sr;
        for (;;) {
            if (f == (int)(to & 7) && r == (int)(to >> 4)) return 1;
            if (b[(r << 4) | f] != EMPTY) return 0;
            f += sf; r += sr;
        }
    }
}

/* 1 = il pezzo (stesso tipo/colore) su sq puo' raggiungere `to`
 * pseudo-legalmente (senza test di auto-scacco) */
static int reaches(const u8 *b, u8 sq, u8 to, u8 pt)
{
    int df = (int)(to & 7) - (int)(sq & 7);
    int dr = (int)(to >> 4) - (int)(sq >> 4);
    int adf = df < 0 ? -df : df, adr = dr < 0 ? -dr : dr;

    if (sq == to) return 0;
    if (b[to] != EMPTY && PT(b[to]) == KING) return 0;   /* mai sulla casa del re */
    switch (pt) {
        case KNIGHT:
            return (adf == 1 && adr == 2) || (adf == 2 && adr == 1);
        case BISHOP:
            return slide_reaches(b, sq, to, 1, 0);
        case ROOK:
            return slide_reaches(b, sq, to, 0, 1);
        case QUEEN:
            return slide_reaches(b, sq, to, 1, 1);
        case KING:
            return (adf <= 1 && adr <= 1);
        case PAWN: {
            int fwd = (PC(b[sq]) == WHITE) ? 1 : -1;
            int from = sq >> 4;
            if (df == 0 && dr == fwd && b[to] == EMPTY) return 1;
            if (df == 0 && dr == 2 * fwd && (from == 1 || from == 6) &&
                b[(int)sq + 16 * fwd] == EMPTY && b[to] == EMPTY) return 1;
            if ((df == 1 || df == -1) && dr == fwd && b[to] != EMPTY) return 1;
            return 0;
        }
        default: return 0;
    }
}

/* SAN standard della mossa m, calcolata sulla posizione corrente del
 * motore (PRIMA di applicarla) e SENZA suffissi +/# */
static void san_of_move(u16 m, char *buf)
{
    u8 from = M_FROM(m), to = M_TO(m), pr = M_PROMO(m);
    u8 p = board[from], pt = PT(p), pc = PC(p);
    u8 others[16], no = 0;
    u8 i, n = legal_moves(list);
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
            u8 f = M_FROM(list[i]);
            if (M_TO(list[i]) == to && f != from &&
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
    int i, len;
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

    i = 0;
    if (s[0] == 'K') { piece = KING;  i = 1; }
    else if (s[0] == 'Q') { piece = QUEEN; i = 1; }
    else if (s[0] == 'R') { piece = ROOK;  i = 1; }
    else if (s[0] == 'B') { piece = BISHOP; i = 1; }
    else if (s[0] == 'N') { piece = KNIGHT; i = 1; }

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

    n = legal_moves(list);
    for (k = 0; k < n; k++) {
        u16 m = list[k];
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

/* ---------------- main ---------------------------------------------- */

static void emit_header(FILE *out, char names[][40], u8 *res, u16 *lens,
                        u16 *offs, u16 *moves, int ngames, int nmoves);

int main(int argc, char **argv)
{
    FILE *in, *out;
    char line[8192], names[64][40];
    u8 res[64];
    u16 lens[64], offs[64];
    u16 moves[4096];
    int ng = 0, nm = 0, warn = 0;

    if (argc < 3) {
        fprintf(stderr, "uso: %s <partite.txt> <output.h>\n", argv[0]);
        return 1;
    }
    in = fopen(argv[1], "r");
    if (!in) { perror(argv[1]); return 1; }

    init_engine();

    while (fgets(line, sizeof(line), in)) {
        char name[40], resstr[16], sans[8192];
        int gamenum;
        char *tok;

        if (sscanf(line, " %39[^\n]", name) != 1) continue;
        if (!fgets(line, sizeof(line), in) ||
            sscanf(line, " %15[^\n]", resstr) != 1) { fprintf(stderr, "file malformato\n"); return 1; }
        if (!fgets(line, sizeof(line), in) ||
            sscanf(line, " %8191[^\n]", sans) != 1) { fprintf(stderr, "file malformato\n"); return 1; }
        (void)fgets(line, sizeof(line), in);    /* riga vuota */

        gamenum = ng;
        strncpy(names[gamenum], name, sizeof(names[gamenum]) - 1);
        names[gamenum][sizeof(names[gamenum]) - 1] = 0;
        res[gamenum] = (u8)(strcmp(resstr, "1-0") == 0 ? 0 :
                            strcmp(resstr, "0-1") == 0 ? 1 : 2);

        new_game();
        offs[gamenum] = (u16)nm;
        lens[gamenum] = 0;

        tok = strtok(sans, " \t");
        while (tok) {
            u16 m = match_san(tok);
            char want[16], got[16], wn[16];
            if (m == MOVE_NONE) {
                fprintf(stderr, "ERRORE %s: mossa illegale/non trovata: %s "
                        "(semimossa %d)\n", name, tok, lens[gamenum]);
                return 1;
            }
            san_of_move(m, got);
            norm(tok, want);
            /* nel caso "O-O" tengo la forma del PGN */
            if (strcmp(got, want) != 0) {
                printf("NOTA %s: SAN '%s' ricalcolata '%s'\n", name, want, got);
                warn++;
            }
            (void)wn;
            moves[nm++] = m;
            lens[gamenum]++;
            commit_move(m);
            tok = strtok(NULL, " \t");
        }
        ng++;
    }
    fclose(in);

    out = fopen(argv[2], "w");
    if (!out) { perror(argv[2]); return 1; }
    emit_header(out, names, res, lens, offs, moves, ng, nm);
    fclose(out);
    printf("%d partite, %d semimosse, %d note SAN\n", ng, nm, warn);
    return 0;
}

static void emit_header(FILE *out, char names[][40], u8 *res, u16 *lens,
                        u16 *offs, u16 *moves, int ngames, int nmoves)
{
    int i, j;
    fprintf(out,
        "/* famous_games.h - partite famose in formato motore (u16).\n"
        " * GENERATO da tools/pgn2moves.c: NON modificare a mano.\n"
        " * Ogni partita e' stata validata mossa per mossa con legal_moves().\n"
        " */\n"
        "#ifndef FAMOUS_GAMES_H\n"
        "#define FAMOUS_GAMES_H\n\n"
        "#include \"chess.h\"\n\n"
        "#define FAME_COUNT %d\n\n"
        "static const char * const fameName[FAME_COUNT] = {\n", ngames);
    for (i = 0; i < ngames; i++)
        fprintf(out, "    \"%s\",\n", names[i]);
    fprintf(out, "};\n\n/* 0 = 1-0, 1 = 0-1, 2 = patta */\n"
                 "static const u8 fameResult[FAME_COUNT] = { ");
    for (i = 0; i < ngames; i++)
        fprintf(out, "%d%s", res[i], i + 1 < ngames ? ", " : "");
    fprintf(out, " };\n\nstatic const u16 fameLen[FAME_COUNT] = { ");
    for (i = 0; i < ngames; i++)
        fprintf(out, "%d%s", lens[i], i + 1 < ngames ? ", " : "");
    fprintf(out, " };\n\nstatic const u16 fameOff[FAME_COUNT] = { ");
    for (i = 0; i < ngames; i++)
        fprintf(out, "%d%s", offs[i], i + 1 < ngames ? ", " : "");
    fprintf(out, " };\n\nstatic const u16 fameMoves[%d] = {\n", nmoves);
    for (i = 0; i < ngames; i++) {
        fprintf(out, "    /* %s */\n", names[i]);
        for (j = 0; j < lens[i]; j++) {
            fprintf(out, "0x%04X,", moves[offs[i] + j]);
            if ((j % 8) == 7 || j + 1 == lens[i]) fprintf(out, "\n");
        }
    }
    fprintf(out, "};\n\n#endif /* FAMOUS_GAMES_H */\n");
}
