/* test_timeout.c - verifica in VICE della scala del timeout di comm_rx_byte()
 *
 * Il ring buffer di ricezione e' vuoto (nessuna cartuccia: nessun NMI), quindi
 * comm_rx_byte() si blocca nel loop di attesa e ne esce solo per timeout.
 * Si misura quante volte cambia il byte basso del jiffy clock ($A0, 1/60 s)
 * durante la chiamata e si confronta con il valore atteso ms>>4 + 1..3.
 *
 * Serve a non ripetere l'errore "$A1"/"$A2": con il byte sbagliato i tick
 * sarebbero 256x/65536x meno frequenti, cioe' il test non finirebbe mai.
 *
 * Build:  cl65 -t c64 -o test_timeout.prg test_timeout.c comm.s
 * Run:    x64sc -warp -limitcycles 20000000 -autostart test_timeout.prg \
 *               -exitscreenshot shot.png
 *         border VERDE = tutti i test ok, ROSSO = almeno uno fuori tolleranza.
 */
#include <stdio.h>
#include <conio.h>
#include "comm.h"

#define BORDER (*(unsigned char *)0xD020)
#define BGCOL  (*(unsigned char *)0xD021)

/* jiffy clock: 16 bit bassi = $A2 (LSB, 1/60 s) + $A1 (ogni ~4.27 s).
 * Attenzione all'ordine: sul C64 il clock e' big-endian ($A0 = MSB). */
static unsigned int now_jif(void)
{
    unsigned char hi, lo, hi2;
    do {
        hi  = *(unsigned char *)0xA1;
        lo  = *(unsigned char *)0xA2;
        hi2 = *(unsigned char *)0xA1;
    } while (hi != hi2);
    return ((unsigned int)hi << 8) | (unsigned int)lo;
}

static unsigned char check(unsigned int ms)
{
    unsigned int expect, t0, ticks;
    int r;

    expect = (ms >> 4) + 1;             /* _rx_tmo + 1 cambi di $A0 */
    t0 = now_jif();
    r  = comm_rx_byte(ms);
    ticks = now_jif() - t0;

    printf("%5u ms -> %d in %3u tick (attesi %u..%u)\n",
           ms, r, ticks, expect, expect + 2);

    if (r != -1) return 0;              /* doveva andare in timeout */
    if (ticks < expect || ticks > expect + 2) return 0;
    return 1;
}

int main(void)
{
    static const unsigned int tmo[4] = { 100, 500, 2000, 10000 };
    unsigned char ok = 1, i;

    BORDER = 0;
    BGCOL  = 0;
    clrscr();
    printf("test timeout comm_rx_byte (jiffy 60 Hz = $A2)\n\n");

    for (i = 0; i < 4; ++i)
        if (!check(tmo[i])) ok = 0;

    printf("\n%s\n", ok ? "TUTTI OK" : "FALLITO");
    BORDER = ok ? 5 : 2;                /* verde / rosso */
    return 0;
}
