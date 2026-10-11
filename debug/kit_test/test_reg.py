import socket
import time

s = socket.create_connection(('127.0.0.1', 15002), timeout=3)
s.settimeout(3)

# Send 'g' packet
s.sendall(b'$g#67')
import time
time.sleep(0.3)

buf = b''
while True:
    c = b''
    try:
        c = s.recv(4096)
    except:
        break
    if not c:
        break
    if b'$' in buf and b'#' in buf:
        break
    buf += s.recv(4096)
    if b'$' in buf and b'#' in buf:
        break

i = buf.find(b'$')
j = buf.find(b'#', buf.find(b'$'))
if j > 0:
    payload = buf[buf.find(b'$')+1:j].decode()
    print('len:', len(payload), 'words:', len(payload)//8)
    print(repr(payload[:200]))
