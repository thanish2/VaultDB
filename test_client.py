import socket
import struct

def send_message(sock, text):
    data = text.encode('utf-8')
    length = struct.pack('!I', len(data))  # '!I' = network byte order, unsigned int
    sock.sendall(length)
    sock.sendall(data)

def receive_message(sock):
    length_bytes = sock.recv(4)
    length = struct.unpack('!I', length_bytes)[0]
    data = b''
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        data += chunk
    return data.decode('utf-8')

s = socket.create_connection(('127.0.0.1', 5000))

print("Sending: PUT|testkey|hello")
send_message(s, "PUT|testkey|hello")
response = receive_message(s)
print("Received:", response)

print("Sending: GET|testkey")
send_message(s, "GET|testkey")
response = receive_message(s)
print("Received:", response)

print("Sending: GET|nonexistent")
send_message(s, "GET|nonexistent")
response = receive_message(s)
print("Received:", response)

s.close()