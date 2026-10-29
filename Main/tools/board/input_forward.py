"""Keyboard/XInput snapshots: legacy ten-bit and checksum-framed fourteen-bit input."""

import ctypes


def crc7(lo,hi):
    crc=0
    for value in (lo,hi):
        for bit in range(6,-1,-1):
            feedback=((crc>>6)^(value>>bit))&1
            crc=((crc<<1)&0x7f)^(9 if feedback else 0)
    return crc


def packet(keys):
    if not 0 <= keys <= 0x3fff:
        raise ValueError("key snapshot must fit fourteen bits")
    if keys > 0x3ff:
        lo,hi=keys&0x7f,(keys>>7)&0x7f
        return bytes((0xff,0x7f,lo,hi,crc7(lo,hi)))
    check = (keys ^ (keys >> 4) ^ (keys >> 8)) & 15
    return bytes((0x80 | (keys & 0x7f), (check << 3) | (keys >> 7)))


class _Gamepad(ctypes.Structure):
    _fields_ = [("buttons", ctypes.c_ushort), ("left_trigger", ctypes.c_ubyte),
                ("right_trigger", ctypes.c_ubyte), ("lx", ctypes.c_short),
                ("ly", ctypes.c_short), ("rx", ctypes.c_short), ("ry", ctypes.c_short)]


class _State(ctypes.Structure):
    _fields_ = [("packet_number", ctypes.c_ulong), ("pad", _Gamepad)]


class InputForwarder:
    # board.h: up, down, left, right, bomb, slow, swap, mode1..3
    _keys = ((0x26, 0), (ord("W"), 0), (0x28, 1), (ord("S"), 1),
             (0x25, 2), (ord("A"), 2), (0x27, 3), (ord("D"), 3),
             (0x20, 4), (0xA0, 5), (0x09, 6),
             (ord("1"), 7), (ord("2"), 8), (ord("3"), 9),
             (0x0d, 10), (0x1b, 11), (0x08, 11), (ord("C"), 12))

    def __init__(self):
        self.user32 = ctypes.windll.user32
        self.user32.GetForegroundWindow.restype = ctypes.c_void_p
        self.focus = self.user32.GetForegroundWindow()
        self.xinput = None
        for name in ("xinput1_4", "xinput1_3", "xinput9_1_0"):
            try:
                self.xinput = ctypes.WinDLL(name)
                break
            except OSError:
                pass
        self.last = None
        self.last_sent = 0.0

    def sample(self):
        bits = 0
        if self.focus and self.user32.GetForegroundWindow() == self.focus:
            for vk, index in self._keys:
                if vk==0x0d and self.user32.GetAsyncKeyState(0x12)&0x8000:
                    continue
                if self.user32.GetAsyncKeyState(vk) & 0x8000:
                    bits |= 1 << index
            if self.user32.GetAsyncKeyState(ord("4")) & 0x8000:
                bits |= 0x380
        if self.xinput is not None:
            state = _State()
            if self.xinput.XInputGetState(0, ctypes.byref(state)) == 0:
                b = state.pad.buttons
                for mask, index in ((0x0001, 0), (0x0002, 1), (0x0004, 2),
                                    (0x0008, 3), (0x1000, 4), (0x0100, 5),
                                    (0x0010, 10), (0x0020, 11), (0x0200, 12),
                                    (0x2000, 6)):
                    if b & mask:
                        bits |= 1 << index
                if state.pad.ly > 16000:
                    bits |= 1 << 0
                if state.pad.ly < -16000:
                    bits |= 1 << 1
                if state.pad.lx < -16000:
                    bits |= 1 << 2
                if state.pad.lx > 16000:
                    bits |= 1 << 3
        return bits

    def send(self, port, now):
        keys = self.sample()
        if keys != self.last or now - self.last_sent >= 0.1:
            port.write(packet(keys))
            self.last, self.last_sent = keys, now

    @staticmethod
    def release(port):
        port.write(packet(0))
