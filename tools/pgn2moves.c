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
#include "san_tool.h"

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

        if (line[0] == '#') continue;           /* commenti nell'input */
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
