from pathlib import Path
import socket,subprocess,time
p=subprocess.Popen([str(Path(__file__).resolve().parent / "native/2v2Server-test8"),'21111'],stdout=subprocess.DEVNULL,stderr=subprocess.STDOUT)
clients=[]
def recvop(s,op):
    end=time.monotonic()+3
    while time.monotonic()<end:
        d=s.recv(4096)
        if d[0]==op:return d
    raise AssertionError(op)
try:
    time.sleep(.3)
    for i in range(4):
        s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.settimeout(3);s.connect(('127.0.0.1',21111));clients.append(s)
        s.send(bytes([1])+bytes(36));assert recvop(s,3)==b'\x03'
        s.send(bytes([5])+bytes.fromhex('41d03b30644174c8c6248ca41544329b')+bytes(8)+bytes([1,2])+b'P'+str(i).encode()+b'\0')
        assert len(recvop(s,6))==145
        s.send(bytes([10,i]));recvop(s,10)
    recvop(clients[0],12)
    payload=b'\x6b'+bytes(range(32));clients[0].send(payload)
    for s in clients[1:]:assert recvop(s,0x6b)==payload
    print('PASS: 9B handshake, four seats, unlock character select, rollback relay')
finally:
    for s in clients:s.close()
    p.terminate();p.wait(timeout=3)
