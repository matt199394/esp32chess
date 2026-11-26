/* comm.h - trasporto byte a byte C64 <-> ESP32 (cartuccia c64esp32cart)
 *
 *  - comm_install()   installa l'handler NMI utente al vettore $0318.
 *  - comm_tx_byte(b)  attende $DF00 bit 7 e scrive b su $DE00.
 *  - comm_rx_byte()   attende un byte dall'ESP32 (ring buffer alimentato
 *                     dall'NMI) e lo restituisce.
 *
 * Le implementazioni sono in comm.s.
 */
#ifndef COMM_H
#define COMM_H

void comm_install(void);

void __fastcall__ comm_tx_byte(unsigned char b);
unsigned char __fastcall__ comm_rx_byte(void);

#endif /* COMM_H */
