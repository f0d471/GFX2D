"""串口结束条件的消费者回归：nmax done 不结束，完整 done 才结束。"""
import importlib.util
from pathlib import Path
import tempfile
import threading
import types
ROOT=Path(__file__).resolve().parents[4]
path=ROOT/'Main/tools/board/board_run.py'
import sys
sys.path.insert(0,str(path.parent))
spec=importlib.util.spec_from_file_location('board_stop_line',path)
b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)
class Fake:
    reads=0
    def __init__(self,*a,**kw):pass
    def reset_input_buffer(self):pass
    def read(self,n):
        chunks=[b'nmax done\r\n',b'fail 0\r\n',b'done\r',b'\n']
        assert Fake.reads<len(chunks),'did not stop on complete done'
        data=chunks[Fake.reads];Fake.reads+=1;return data
    def close(self):pass
b.serial=types.SimpleNamespace(Serial=Fake,SerialException=RuntimeError)
ready=threading.Event();armed=threading.Event();armed.set();result={}
with tempfile.TemporaryDirectory(prefix='f4-stop-line-') as d:
    b.serial_worker('fake',115200,str(Path(d)/'uart.log'),2,None,ready,armed,result,stop_line='done')
assert result.get('stopped') and Fake.reads==4,result
print('F4 UART STOP PASS nmax_done=ignored partial_line=wait complete_done=accepted')
