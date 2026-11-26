/* comm.h - trasporto byte a byte C64 <-> ESP32 (cartuccia c64esp32cart)
 *
 *  - comm_install()   installa l'handler NMI utente al vettore $0318.
 *  - comm_tx_byte(b)  attende $DF00 bit 7, scrive b su $DE00 e attende l'ack
 *                     (D7 abbassato dall'ISR dell'ESP32); ritorna 0 ok,
 *                     -1 se l'ack non arriva.
 *  - comm_rx_byte(ms) attende un byte dall'ESP32 (ring buffer alimentato
 *                     dall'NMI) per al piu' ms millisecondi (0 = attesa
 *                     infinita) e lo restituisce; su timeout svuota il
 *                     ring buffer (resync) e ritorna -1.
 *  - comm_rx_flush()  scarta i byte ricevuti ma non consumati.
 *
 * Le implementazioni sono in comm.s.
 */
#ifndef COMM_H
#define COMM_H

void comm_install(void);
void comm_rx_flush(void);

int  __fastcall__ comm_tx_byte(unsigned char b);
int  __fastcall__ comm_rx_byte(unsigned int timeout_ms);

#endif /* COMM_H */
