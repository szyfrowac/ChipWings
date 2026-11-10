"""Console access shared by the test scripts: a Renode socket terminal or a board serial port.

    from cwconsole import Console, add_target_args

    ap = argparse.ArgumentParser()
    add_target_args(ap)                       # --target renode|board, --nsh-port, --linux-port, --nsh-serial, ...
    args = ap.parse_args()
    nsh = Console.open(args, "nsh")           # NuttX / PX4 shell (MMUART1)
    lx = Console.open(args, "linux")          # Linux console (MMUART3)

On the board the two consoles are separate USB serial ports of the Icicle Kit's FTDI bridge
(typically /dev/ttyUSB1 = MMUART1, /dev/ttyUSB3 = MMUART3; the numbering depends on enumeration order).
"""
import re
import socket
import time

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


def add_target_args(ap, nsh_port=33457, linux_port=33458):
    ap.add_argument("--target", choices=["renode", "board"], default="renode",
                    help="renode: telnet sockets of the simulation; board: serial ports of the Icicle Kit")
    ap.add_argument("--nsh-port", type=int, default=nsh_port, help="Renode: NuttX NSH socket")
    ap.add_argument("--linux-port", type=int, default=linux_port, help="Renode: Linux console socket")
    ap.add_argument("--nsh-serial", default="/dev/ttyUSB1", help="board: MMUART1 (NuttX) serial device")
    ap.add_argument("--linux-serial", default="/dev/ttyUSB3", help="board: MMUART3 (Linux) serial device")
    ap.add_argument("--baud", type=int, default=115200)


class Console:
    def __init__(self, recv, send, close):
        self._recv, self._send, self.close = recv, send, close

    @classmethod
    def open(cls, args, which):
        """which: 'nsh' or 'linux'."""
        if args.target == "renode":
            s = socket.create_connection(("127.0.0.1", getattr(args, f"{which}_port")))
            s.settimeout(0.3)

            def recv():
                try:
                    return s.recv(65536)
                except socket.timeout:
                    return b""
            return cls(recv, s.sendall, s.close)
        import serial  # pyserial, only needed on the board
        p = serial.Serial(getattr(args, f"{which}_serial"), args.baud, timeout=0.3)
        return cls(lambda: p.read(65536), p.write, p.close)

    def read_until(self, pattern, timeout):
        """Read until the regex matches the (escape-stripped) output. Returns (text, match or None)."""
        raw, end = "", time.time() + timeout
        while time.time() < end:
            raw += self._recv().decode(errors="replace")
            clean = ANSI.sub("", raw).replace("\r", "")
            m = re.search(pattern, clean)
            if m:
                return clean, m
        return ANSI.sub("", raw).replace("\r", ""), None

    def send(self, line):
        self._send(line.encode() + b"\r")

    def login(self):
        """Log into Linux as root/root if it is at a login prompt; return once at a shell prompt."""
        self.send("")
        out, _ = self.read_until(r"login: $|# $", 30)
        if "login:" in out:
            self.send("root")
            self.read_until(r"Password: $", 10)
            self.send("root")
            self.read_until(r"# $", 30)
