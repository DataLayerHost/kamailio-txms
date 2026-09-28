import http.server
import json
import os
from pathlib import Path
import ssl
import tempfile
import subprocess
import sys
import threading
import time
class Handler(http.server.BaseHTTPRequestHandler):
	def log_message(self, *args): pass
	def do_POST(self):
		request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
		assert request == {'jsonrpc': '2.0', 'method': 'operator_broadcast', 'id': '42', 'params': ['0xabcd']}
		kind = self.path[1:]
		if kind == 'basic': assert self.headers['Authorization'] == 'Basic dXNlcjpwYXNz'
		if kind == 'bearer': assert self.headers['Authorization'] == 'Bearer test-only-token'
		payload = {'jsonrpc': '2.0', 'id': '42', 'result': '0xhash'}
		if kind == 'error': payload = {'jsonrpc': '2.0', 'id': '42', 'error': {'code': -32000, 'message': 'rejected'}}
		if kind == 'wrong-id': payload['id'] = '41'
		if kind == 'both': payload['error'] = {'code': 1, 'message': 'bad'}
		if kind == 'null': payload['result'] = None
		if kind == 'timeout': time.sleep(0.5)
		data = json.dumps(payload).encode()
		if kind == 'malformed': data = b'{bad'
		if kind == 'trailing': data += b'garbage'
		if kind == 'oversized': data = b'x' * 70000
		self.send_response(500 if kind == 'http' else 200)
		self.send_header('Content-Length', str(len(data))); self.end_headers()
		try: self.wfile.write(data)
		except (BrokenPipeError, ConnectionResetError): pass
server = http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
	for kind in ['success','error','http','timeout','malformed','wrong-id','both','null','trailing','oversized']:
		result = subprocess.check_output([sys.argv[1],f'http://127.0.0.1:{server.server_port}/{kind}'],text=True).strip()
		assert result == str({'success': 1, 'error': 2}.get(kind, 3)), (kind,result)
	print('10 RPC integration cases passed')
finally: server.shutdown(); server.server_close()

for kind,values in [('basic',{'TEST_RPC_USERNAME':'user','TEST_RPC_PASSWORD':'pass'}),('bearer',{'TEST_RPC_TOKEN':'test-only-token'})]:
	server = http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
	threading.Thread(target=server.serve_forever,daemon=True).start()
	try:
		result = subprocess.check_output([sys.argv[1],f'http://127.0.0.1:{server.server_port}/{kind}'],env={**os.environ,**values},text=True).strip()
		assert result=='1',(kind,result)
	finally: server.shutdown(); server.server_close()
with tempfile.TemporaryDirectory(prefix='txms-tls-') as folder:
	cert,key=Path(folder,'cert.pem'),Path(folder,'key.pem')
	subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1','-subj','/CN=localhost','-keyout',str(key),'-out',str(cert)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
	server = http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
	ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert,key)
	server.socket=ctx.wrap_socket(server.socket,server_side=True)
	threading.Thread(target=server.serve_forever,daemon=True).start()
	try:
		url=f'https://127.0.0.1:{server.server_port}/success'
		assert subprocess.check_output([sys.argv[1],url],text=True).strip()=='3'
		assert subprocess.check_output([sys.argv[1],url,'0'],text=True).strip()=='1'
	finally: server.shutdown(); server.server_close()
print('Basic/bearer authentication and TLS verification cases passed')
