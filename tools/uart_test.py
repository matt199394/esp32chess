#!/usr/bin/env python3
"""Prova del motore esp32chess via seriale (roadmap punto 1 di AGENTS.md).

Parla il protocollo di docs/PROTOCOL.md sulla USB dell'ESP32 (115200 8N1).

Uso:
    python3 tools/uart_test.py /dev/ttyACM0            # umano = bianco
    python3 tools/uart_test.py /dev/ttyACM0 black      # umano = nero

Richiede pyserial:  pip install pyserial
"""
import sys
import serial

# costanti del protocollo (in sync con protocol.h)
CMD_NEW_GAME, CMD_SET_COLOR, CMD_SET_LEVEL = 0x4E, 0x43, 0x4C
CMD_MOVE, CMD_UNDO, CMD_QUIT = 0x4D, 0x55, 0x51
RSP_ILLEGAL, RSP_ACK, RSP_BEST_MOVE, RSP_GAME_OVER = 0x00, 0x01, 0x02, 0x03
OVER = {1: "CHECKMATE", 2: "STALEMATE", 3: "50 MOVE DRAW", 4: "REPETITION", 5: "MATERIAL"}

MOVE_NONE = 0xFFFF


def parse_move(s):
    """e2e4 / e2-e4 / e7e8q -> mossa 16 bit (0x88, promo: 0=N 1=B 2=R 3=Q)."""
    s = s.strip().lower()
    if len(s) < 4:
        return MOVE_NONE
    f0, r0 = ord(s[0]) - 97, ord(s[1]) - 49
    if s[2] in "- ":
        s = s[0:2] + s[3:]
    f1, r1 = ord(s[2]) - 97, ord(s[3]) - 49
    promo = 0
    if len(s) > 4:
        promo = {"n": 0, "b": 1, "r": 2, "q": 3}[s[4]]
    if not (0 <= f0 < 8 and 0 <= r0 < 8 and 0 <= f1 < 8 and 0 <= r1 < 8):
        return MOVE_NONE
    return (r0 << 4 | f0) | ((r1 << 4 | f1) << 7) | (promo << 14)


def move_str(m):
    f0, t0 = m & 0x7F, (m >> 7) & 0x7F
    return f"{chr(97 + (f0 & 7))}{1 + (f0 >> 4)}{chr(97 + (t0 & 7))}{1 + (t0 >> 4)}"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    port = sys.argv[1]
    human = 1 if (len(sys.argv) > 2 and sys.argv[2].lower() in ("black", "b")) else 0

    ser = serial.Serial(port, 115200, timeout=5)

    def rd():
        b = ser.read(1)
        if not b:
            raise SystemExit("timeout seriale")
        return b[0]

    ser.write(bytes([CMD_SET_COLOR, human])); assert rd() == RSP_ACK
    ser.write(bytes([CMD_SET_LEVEL, 1]));      assert rd() == RSP_ACK
    ser.write(bytes([CMD_NEW_GAME]));          assert rd() == RSP_ACK
    print("nuova partita, umano =", "nero" if human else "bianco")

    def recv_computer_move(rsp):
        if rsp == RSP_GAME_OVER:
            reason, winner = rd(), rd()
            print("GAME_OVER:", OVER.get(reason, reason), "winner", winner)
            return True
        assert rsp == RSP_BEST_MOVE, f"risposta inattesa {rsp}"
        m = rd() | (rd() << 8)
        score = rd() | (rd() << 8)
        if score >= 0x8000:
            score -= 0x10000
        depth, flags = rd(), rd()
        print(f"AI: {move_str(m)}  score={score}  depth={depth}"
              + ("  CHECK!" if flags & 1 else ""))
        if flags & 2:
            reason, winner = rd(), rd()
            print("GAME_OVER:", OVER.get(reason, reason), "winner", winner)
            return True
        return False

    if human == 1:                       # il computer (bianco) apre
        if recv_computer_move(rd()):
            return

    while True:
        line = input("move: ").strip()
        if line in (".", "q", "quit"):
            ser.write(bytes([CMD_QUIT])); rd()
            break
        if line in ("u", "undo"):
            ser.write(bytes([CMD_UNDO])); assert rd() == RSP_ACK
            print("undo")
            continue
        m = parse_move(line)
        if m == MOVE_NONE:
            print("mossa non valida")
            continue
        ser.write(bytes([CMD_MOVE, m & 0xFF, (m >> 8) & 0xFF]))
        rsp = rd()
        if rsp == RSP_ILLEGAL:
            print("ILLEGAL")
            continue
        if rsp == RSP_GAME_OVER:
            reason, winner = rd(), rd()
            print("GAME_OVER:", OVER.get(reason, reason), "winner", winner)
            break
        assert rsp == RSP_BEST_MOVE, f"risposta inattesa {rsp}"
        if recv_computer_move(rsp):
            break


if __name__ == "__main__":
    main()
