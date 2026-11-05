import ctypes, sys
def crc7_old(lo,hi):
    crc=0
    for value in (lo,hi):
        for bit in range(6,-1,-1):
            feedback=((crc>>6)^(value>>bit))&1
            crc=((crc<<1)&0x7f)^(9 if feedback else 0)
    return crc
def packet_old(keys):
    if keys<=0x3ff:
        check=(keys^(keys>>4)^(keys>>8))&15
        return bytes((0x80|(keys&0x7f),(check<<3)|(keys>>7)))
    lo,hi=keys&0x7f,(keys>>7)&0x7f
    return bytes((0xff,0x7f,lo,hi,crc7_old(lo,hi)))
sys.path.insert(0,'/mnt/e/Workspace/FPGA/Efinix/Main/tools/board')
from input_forward import packet
# 1) 新 packet 与旧 packet 全域逐字节相同(bit14=0 时)
for key in range(0x4000):
    assert packet(key)==packet_old(key), hex(key)
print('byte-identical frames for keys <0x4000: PASS')
class Decoder(ctypes.Structure):
    _fields_=[('state',ctypes.c_uint8),('header',ctypes.c_uint8),('lo',ctypes.c_uint8),('hi',ctypes.c_uint8)]
lib=ctypes.CDLL(r'/mnt/e/Workspace/Temp/zc-sh3/libkeypacket.so')
lib.key_packet_feed.argtypes=[ctypes.POINTER(Decoder),ctypes.c_uint8,ctypes.POINTER(ctypes.c_uint32)]
def roundtrip(pkt,key):
    d=Decoder(); value=ctypes.c_uint32()
    acc=[lib.key_packet_feed(ctypes.byref(d),b,ctypes.byref(value)) for b in pkt]
    return sum(acc)==1 and acc[-1]==1 and value.value==key
# 2) 新 packet -> 新解码器 32768 全跑
for key in range(32768):
    assert roundtrip(packet(key),key), ('new',key)
print('new packet -> new decoder 32768: PASS')
# 3) 旧 packet -> 新解码器 16384 全跑(旧工具兼容)
for key in range(0x4000):
    assert roundtrip(packet_old(key),key), ('old',key)
print('old packet -> new decoder 16384: PASS')
