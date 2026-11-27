# Makefile - build UI C64 (cc65)
#
# make          -> chessui.prg
# make test-timeout -> regressione del timeout di comm_rx_byte in VICE
# make clean

CC65 = cl65
CFLAGS = -t c64 -Oirs -m chessui.map -Wl -D,__STACKSIZE__=0x1000

all: chessui.prg

chessui.prg: chessui.c comm.s comm.h protocol.h font_writer.h
	$(CC65) $(CFLAGS) -o chessui.prg chessui.c comm.s

# Regressione del timeout: misura il jiffy clock in VICE (bordo verde = ok).
# Serve a non riusare $A0/$A1 al posto di $A2 (clock big-endian, vedi comm.s).
test-timeout: test_timeout.prg
	python3 ../tools/vice_test_timeout.py

test_timeout.prg: test_timeout.c comm.s comm.h
	$(CC65) -t c64 -o test_timeout.prg test_timeout.c comm.s

clean:
	rm -f chessui.prg chessui.o comm.o test_timeout.prg test_timeout.o
