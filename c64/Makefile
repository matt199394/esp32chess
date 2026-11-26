# Makefile - build UI C64 (cc65)
#
# make          -> chessui.prg
# make clean

CC65 = cl65
CFLAGS = -t c64 -Oirs -Wl -D,__STACKSIZE__=0x1000

all: chessui.prg

chessui.prg: chessui.c comm.s comm.h protocol.h
	$(CC65) $(CFLAGS) -o chessui.prg chessui.c comm.s

clean:
	rm -f chessui.prg chessui.o comm.o
