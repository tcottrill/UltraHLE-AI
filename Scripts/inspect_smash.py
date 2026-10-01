"""Temporary read-only live emulator diagnostic."""
import ctypes as c
from ctypes import wintypes as w
import struct,sys,time
from pathlib import Path
k=c.WinDLL('kernel32',use_last_error=True)
d=c.WinDLL('dbghelp',use_last_error=True)
k.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD];k.OpenProcess.restype=w.HANDLE
k.ReadProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.c_void_p]
d.SymInitialize.argtypes=[w.HANDLE,c.c_char_p,w.BOOL]
d.SymFromName.argtypes=[w.HANDLE,c.c_char_p,c.c_void_p]
class Symbol(c.Structure):
    _fields_=[('SizeOfStruct',w.ULONG),('TypeIndex',w.ULONG),('Reserved',c.c_ulonglong*2),('Index',w.ULONG),('Size',w.ULONG),('ModBase',c.c_ulonglong),('Flags',w.ULONG),('Value',c.c_ulonglong),('Address',c.c_ulonglong),('Register',w.ULONG),('Scope',w.ULONG),('Tag',w.ULONG),('NameLen',w.ULONG),('MaxNameLen',w.ULONG),('Name',c.c_char*1024)]
h=k.OpenProcess(0x410,False,int(sys.argv[1]))
if not h: raise c.WinError(c.get_last_error())
if not d.SymInitialize(h,str(Path('Build').resolve()).encode(),True):raise c.WinError(c.get_last_error())
def symbol(name):
    s=Symbol();s.SizeOfStruct=88;s.MaxNameLen=1024
    if not d.SymFromName(h,name.encode(),c.byref(s)):raise c.WinError(c.get_last_error())
    return s.Address
def read(addr,n):
    b=c.create_string_buffer(n)
    if not k.ReadProcessMemory(h,addr,b,n,None):raise c.WinError(c.get_last_error())
    return b.raw
st=symbol('st');mem=symbol('mem')
ram=struct.unpack('<Q',read(mem+0x1040028,8))[0]
for i in range(8):
    state=read(st,1616);regs=struct.unpack_from('<32Q',state,8)
    print('pc',hex(struct.unpack_from('<I',state,4)[0]),'ra',hex(regs[31]),'sp',hex(regs[29]))
    time.sleep(.1)
Path('Build/smash2_state.bin').write_bytes(state)
Path('Build/smash2_ram.bin').write_bytes(read(ram,0x400000))
threads=read(symbol('thread'),1712*16)
Path('Build/smash2_threads.bin').write_bytes(threads)
for i in range(16):
    t=threads[i*1712:(i+1)*1712];head=struct.unpack_from('<6I',t)
    if head[4]:print('thread',i,'header',head,'pc',hex(struct.unpack_from('<I',t,28)[0]),'ra',hex(struct.unpack_from('<Q',t,280)[0]),'blocking',struct.unpack_from('<3I',t,1640))
queues=read(symbol('queue'),16468*128)
for i in range(128):
    q=struct.unpack_from('<7I',queues,i*16468)
    if q[0]:print('queue',i,hex(q[0]),q[1:])
