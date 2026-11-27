/*
 * endgames2h.c - genera esp32/endgames.h e c64/endgames.h da
 * tools/endgames.txt, validando posizioni e soluzioni con il motore
 * vero (legal_moves, in_check).
 *
 * Per ogni finale:
 *  - il FEN viene caricato nel motore ( legality: un re per lato, il
 *    lato NON di tratto non deve essere in scacco, mosse legali > 0);
 *  - la soluzione SAN viene applicata mossa per mossa con legal_moves();
 *  - con TARGET > 0 la linea deve terminare con SCACCO MATTO e il
 *    numero di mosse del vincitore deve corrispondere al TARGET.
 *
 * Build (PC): gcc -I esp32 tools/endgames2h.c esp32/chess.c -o /tmp/endgames2h
 * Run:        /tmp/endgames2h tools/endgames.txt esp32/endgames.h c64/endgames.h
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "chess.h"
#include "san_tool.h"

#define MAXEND   16
#define LABELMAX 32

static char  ename[MAXEND][LABELMAX];
static u8    eboard[MAXEND][64];    /* a8..h1 */
static u8    eside[MAXEND];
static u8    ecastle[MAXEND];
static u8    eep[MAXEND];           /* 0xFF = nessuna */
static u8    ewinner[MAXEND];
static u8    etarget[MAXEND];
static int   nend = 0;

static int piece_from_char(char c)
{
    switch (c) {
        case 'P': case 'p': return PAWN;
        case 'N': case 'n': return KNIGHT;
        case 'B': case 'b': return BISHOP;
        case 'R': case 'r': return ROOK;
        case 'Q': case 'q': return QUEEN;
        case 'K': case 'k': return KING;
    }
    return -1;
}

/* carica il FEN nella board del motore; ritorna 0 ok */
static int parse_fen(const char *fen, u8 *castleOut, s8 *epOut, u8 *sideOut)
{
    char buf[128];
    char *f[8];
    int nf = 0, rank = 7, file = 0, i;
    char *p;

    strncpy(buf, fen, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    p = strtok(buf, " \t");
    while (p && nf < 8) { f[nf++] = p; p = strtok(NULL, " \t"); }
    if (nf < 2) return -1;

    for (i = 0; i < 128; i++) board[i] = 0;
    p = f[0];
    while (*p) {
        char c = *p++;
        if (c == '/') { rank--; file = 0; continue; }
        if (c >= '1' && c <= '8') { file += c - '0'; continue; }
        {
            int t = piece_from_char(c);
            u8 col = (c >= 'a' && c <= 'z') ? BLACK : WHITE;
            if (t < 0 || rank < 0 || file > 7) return -1;
            board[(rank << 4) | file] = (u8)(t | (col << 3));
            file++;
        }
    }
    if (rank != 0 || file != 8) return -1;

    *sideOut = (f[1][0] == 'b') ? BLACK : WHITE;

    *castleOut = 0;
    if (nf >= 3 && strcmp(f[2], "-") != 0)
        for (p = f[2]; *p; p++) {
            if (*p == 'K') *castleOut |= C_WK;
            if (*p == 'Q') *castleOut |= C_WQ;
            if (*p == 'k') *castleOut |= C_BK;
            if (*p == 'q') *castleOut |= C_BQ;
        }

    *epOut = -1;
    if (nf >= 4 && f[3][0] >= 'a' && f[3][0] <= 'h')
        *epOut = (s8)(((f[3][1] - '1') << 4) | (f[3][0] - 'a'));

    return 0;
}

/* posizione corrente del motore -> board a8..h1 (64 byte) */
static void board_to_64(u8 *out)
{
    int i;
    for (i = 0; i < 64; i++)
        out[i] = board[(u8)(((7 - (i >> 3)) << 4) | (i & 7))];
}

static int load_position(const char *name, const char *fen)
{
    u8 castle, s;
    s8 ep;
    u8 sq, kw, kb;

    if (parse_fen(fen, &castle, &ep, &s) != 0) {
        fprintf(stderr, "ERRORE %s: FEN non valido: %s\n", name, fen);
        return 1;
    }
    side = s;
    castle = castle;
    epSq = ep;
    halfmove = 0;

    kw = kb = 0;
    for (sq = 0; sq < 128; sq++) {
        if (sq & 0x88) continue;
        if (PT(board[sq]) == KING) {
            kingSq[PC(board[sq])] = sq;
            if (PC(board[sq]) == WHITE) kw++; else kb++;
        }
    }
    if (kw != 1 || kb != 1) {
        fprintf(stderr, "ERRORE %s: %d re bianchi, %d neri\n", name, kw, kb);
        return 1;
    }

    refresh_eval();
    compute_key();
    reset_history();

    /* il lato NON di tratto non deve essere in scacco (posizione illegale) */
    if (in_check_of((u8)(side ^ 1))) {
        fprintf(stderr, "ERRORE %s: il lato non di tratto e' in scacco\n", name);
        return 1;
    }
    if (legal_moves(sanList) == 0) {
        fprintf(stderr, "ERRORE %s: nessuna mossa legale per il lato di tratto\n", name);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    FILE *in, *out;
    char line[8192];
    int warn = 0, i, j;

    if (argc < 4) {
        fprintf(stderr, "uso: %s <endgames.txt> <esp32/endgames.h> <c64/endgames.h>\n",
                argv[0]);
        return 1;
    }
    in = fopen(argv[1], "r");
    if (!in) { perror(argv[1]); return 1; }

    init_engine();

    while (fgets(line, sizeof(line), in)) {
        char name[LABELMAX], fen[128], meta[64], sans[512], *tok;
        int target, plies = 0;
        char wch;
        u8 winner, firstSide, lastMover;

        if (line[0] == '#') continue;
        if (sscanf(line, " %31[^\n]", name) != 1) continue;
        if (!fgets(line, sizeof(line), in) ||
            sscanf(line, " %127[^\n]", fen) != 1) { fprintf(stderr, "file malformato\n"); return 1; }
        if (!fgets(line, sizeof(line), in) ||
            sscanf(line, " %63[^\n]", meta) != 1) { fprintf(stderr, "file malformato\n"); return 1; }
        if (!fgets(line, sizeof(line), in) ||
            sscanf(line, " %511[^\n]", sans) != 1) { fprintf(stderr, "file malformato\n"); return 1; }
        (void)fgets(line, sizeof(line), in);    /* riga vuota */

        if (nend >= MAXEND) { fprintf(stderr, "troppi finali\n"); return 1; }

        if (sscanf(meta, " %d %c", &target, &wch) != 2) {
            fprintf(stderr, "ERRORE %s: riga TARGET illeggibile: %s\n", name, meta);
            return 1;
        }
        winner = (wch == 'b') ? BLACK : WHITE;

        if (load_position(name, fen)) return 1;

        board_to_64(eboard[nend]);
        eside[nend]   = side;
        ecastle[nend] = castle;
        eep[nend]     = (epSq < 0) ? 0xFF : (u8)epSq;
        ewinner[nend] = winner;
        etarget[nend] = (u8)target;
        strncpy(ename[nend], name, LABELMAX - 1);
        ename[nend][LABELMAX - 1] = 0;
        firstSide = side;

        if (strcmp(sans, "-") != 0) {
            tok = strtok(sans, " \t");
            while (tok) {
                u16 m = match_san(tok);
                char want[16], got[16];
                if (m == MOVE_NONE) {
                    fprintf(stderr, "ERRORE %s: mossa illegale/non trovata: %s "
                            "(semimossa %d)\n", name, tok, plies);
                    return 1;
                }
                san_of_move(m, got);
                norm(tok, want);
                if (strcmp(got, want) != 0) {
                    printf("NOTA %s: SAN '%s' ricalcolata '%s'\n", name, want, got);
                    warn++;
                }
                commit_move(m);
                plies++;
                tok = strtok(NULL, " \t");
            }

            /* conta le mosse del vincitore (chi muove per primo alterna) */
            lastMover = (u8)((firstSide + (plies - 1)) & 1);
            if (target > 0) {
                int wPlies = (firstSide == winner) ? (plies + 1) / 2 : plies / 2;
                if (legal_moves(sanList) != 0 || !in_check()) {
                    fprintf(stderr, "ERRORE %s: la soluzione NON termina con scacco matto\n", name);
                    return 1;
                }
                if (lastMover != winner) {
                    fprintf(stderr, "ERRORE %s: l'ultima mossa non e' del vincitore\n", name);
                    return 1;
                }
                if (wPlies != target) {
                    fprintf(stderr, "ERRORE %s: TARGET %d ma la soluzione ne usa %d\n",
                            name, target, wPlies);
                    return 1;
                }
            } else if (lastMover != winner) {
                fprintf(stderr, "NOTA %s: l'ultima mossa della linea non e' del vincitore\n", name);
            }
        } else if (target > 0) {
            fprintf(stderr, "ERRORE %s: TARGET > 0 richiede una soluzione\n", name);
            return 1;
        }

        printf("%2d. %-30s FEN ok, %d semimosse validate\n", nend + 1, name, plies);
        nend++;
    }
    fclose(in);
    if (nend == 0) { fprintf(stderr, "nessun finale nel file\n"); return 1; }

    /* ---- esp32/endgames.h ---- */
    out = fopen(argv[2], "w");
    if (!out) { perror(argv[2]); return 1; }
    fprintf(out,
        "/* endgames.h - finali: posizioni iniziali e metadati.\n"
        " * GENERATO da tools/endgames2h.c: NON modificare a mano.\n"
        " * Board a8..h1 (64 byte), codifica pezzi del motore (chess.h).\n"
        " * Posizioni e soluzioni validate con legal_moves()/in_check().\n"
        " */\n"
        "#ifndef ENDGAMES_H\n"
        "#define ENDGAMES_H\n\n"
        "#include \"chess.h\"\n\n"
        "#define END_COUNT %d\n\n"
        "static const u8 endBoard[END_COUNT][64] = {\n", nend);
    for (i = 0; i < nend; i++) {
        fprintf(out, "    /* %s */\n    {", ename[i]);
        for (j = 0; j < 64; j++)
            fprintf(out, "%d%s", eboard[i][j], j < 63 ? "," : "");
        fprintf(out, "},\n");
    }
    fprintf(out, "};\n\n/* lato di tratto alla posizione iniziale */\n"
                 "static const u8 endSide[END_COUNT] = { ");
    for (i = 0; i < nend; i++) fprintf(out, "%d%s", eside[i], i + 1 < nend ? ", " : "");
    fprintf(out, " };\n\n/* diritti di arrocco */\n"
                 "static const u8 endCastle[END_COUNT] = { ");
    for (i = 0; i < nend; i++) fprintf(out, "%d%s", ecastle[i], i + 1 < nend ? ", " : "");
    fprintf(out, " };\n\n/* casa en passant (0xFF = nessuna) */\n"
                 "static const u8 endEp[END_COUNT] = { ");
    for (i = 0; i < nend; i++) fprintf(out, "0x%02X%s", eep[i], i + 1 < nend ? ", " : "");
    fprintf(out, " };\n\n/* il lato che gioca l'umano (quello che vince) */\n"
                 "static const u8 endWinner[END_COUNT] = { ");
    for (i = 0; i < nend; i++) fprintf(out, "%d%s", ewinner[i], i + 1 < nend ? ", " : "");
    fprintf(out, " };\n\n/* 0 = tecnica, N = matto in N mosse dell'umano */\n"
                 "static const u8 endTarget[END_COUNT] = { ");
    for (i = 0; i < nend; i++) fprintf(out, "%d%s", etarget[i], i + 1 < nend ? ", " : "");
    fprintf(out, " };\n\n#endif /* ENDGAMES_H */\n");
    fclose(out);

    /* ---- c64/endgames.h (tipi indipendenti dal motore) ---- */
    out = fopen(argv[3], "w");
    if (!out) { perror(argv[3]); return 1; }
    fprintf(out,
        "/* endgames.h - finali: etichette e board per la UI C64.\n"
        " * GENERATO da tools/endgames2h.c: NON modificare a mano.\n"
        " * Board a8..h1 (64 byte), codifica pezzi (t | colore<<3) come in\n"
        " * chessui.c. Deve restare allineato con esp32/endgames.h.\n"
        " */\n"
        "#ifndef C64_ENDGAMES_H\n"
        "#define C64_ENDGAMES_H\n\n"
        "#define END_COUNT %d\n\n"
        "static const char endLabel[END_COUNT][%d] = {\n", nend, LABELMAX);
    for (i = 0; i < nend; i++)
        fprintf(out, "    \"%s\",\n", ename[i]);
    fprintf(out, "};\n\nstatic const unsigned char endBoard[END_COUNT][64] = {\n");
    for (i = 0; i < nend; i++) {
        fprintf(out, "    /* %s */\n    {", ename[i]);
        for (j = 0; j < 64; j++)
            fprintf(out, "%d%s", eboard[i][j], j < 63 ? "," : "");
        fprintf(out, "},\n");
    }
    fprintf(out, "};\n\n/* lato di tratto alla posizione iniziale */\n"
                 "static const unsigned char endSide[END_COUNT] = { ");
    for (i = 0; i < nend; i++) fprintf(out, "%d%s", eside[i], i + 1 < nend ? ", " : "");
    fprintf(out, " };\n\n/* il lato che gioca l'umano (quello che vince) */\n"
                 "static const unsigned char endWinner[END_COUNT] = { ");
    for (i = 0; i < nend; i++) fprintf(out, "%d%s", ewinner[i], i + 1 < nend ? ", " : "");
    fprintf(out, " };\n\n#endif /* C64_ENDGAMES_H */\n");
    fclose(out);

    printf("%d finali, %d note SAN\n", nend, warn);
    return 0;
}
