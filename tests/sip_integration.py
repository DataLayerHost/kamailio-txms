"""Real Kamailio + UDP MESSAGE + local HTTP RPC integration; no public RPC calls."""
import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time

requests = []
class RPC(http.server.BaseHTTPRequestHandler):
	def log_message(self, *args): pass
	def do_POST(self):
		data = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
		assert data['method'] == 'operator_broadcast'
		requests.append(data['params'][0])
		body = json.dumps({'jsonrpc': '2.0', 'id': data['id'], 'result': '0xlocal-test-hash'}).encode()
		self.send_response(200); self.send_header('Content-Length',str(len(body))); self.end_headers(); self.wfile.write(body)

root = Path(sys.argv[1]).resolve()
http = http.server.ThreadingHTTPServer(('127.0.0.1',0),RPC)
threading.Thread(target=http.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory(prefix='txms-sip-') as temp:
	folder = Path(temp)
	sock = socket.socket(socket.AF_INET,socket.SOCK_DGRAM); sock.bind(('127.0.0.1',0)); sock.settimeout(2)
	probe = socket.socket(socket.AF_INET,socket.SOCK_DGRAM); probe.bind(('127.0.0.1',0)); port=probe.getsockname()[1]; probe.close()
	config = f'''#!KAMAILIO
log_stderror=yes
fork=yes
children=2
listen=udp:127.0.0.1:{port}
disable_tcp=yes
loadmodule "sl.so"
loadmodule "textops.so"
loadmodule "pv.so"
loadmodule "txms.so"
loadmodule "jsonrpcs.so"
modparam("jsonrpcs", "transport", 4)
modparam("jsonrpcs", "dgram_socket", "{folder}/rpc.sock")
modparam("txms", "multipart_ttl", 3)
modparam("txms", "job_ttl", 3)
modparam("txms", "cleanup_interval", 1)
modparam("txms", "rpc_url", "http://127.0.0.1:{http.server_port}/rpc")
modparam("txms", "rpc_method", "operator_broadcast")
modparam("txms", "sms_framing", 0)
request_route {{
	if (is_method("MESSAGE")) {{
		if ($hdr(X-Inspect) == "yes") {{
			if (!txms_process()) {{ sl_send_reply("400", "Rejected"); exit; }}
			if (txms_is_complete()) {{
				if (!txms_get_transaction("$var(tx)")) {{ sl_send_reply("500", "Inspection failed"); exit; }}
				if (!txms_submit()) {{ sl_send_reply("503", "Queue failed"); exit; }}
			}}
			sl_send_reply("200", "Accepted"); exit;
		}}
		if (txms_process_and_submit()) {{ sl_send_reply("200", "Accepted"); exit; }}
		sl_send_reply("400", "Rejected"); exit;
	}}
	sl_send_reply("405", "Method Not Allowed");
}}
'''
	(folder/'kamailio.cfg').write_text(config)
	log = (folder/'server.log').open('w+')
	process = subprocess.Popen([str(root/'src/kamailio'),'-D','-E','-f',str(folder/'kamailio.cfg'),'-L',str(root/'src/modules')],stdout=log,stderr=log,start_new_session=True)
	control = socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM)
	control.bind(str(folder/'client.sock')); control.settimeout(2)
	def stats():
		control.sendto(json.dumps({'jsonrpc':'2.0','id':1,'method':'txms.stats'}).encode(),str(folder/'rpc.sock'))
		response=json.loads(control.recv(65536))
		if 'error' in response: return None
		return response['result']
	serial=0
	def send(body,ct='text/plain',expected=200,inspect=False):
		global serial
		serial+=1
		if isinstance(body,str): body=body.encode()
		message=(f'MESSAGE sip:to@localhost SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:{sock.getsockname()[1]};branch=z9hG4bK-{serial};rport\r\nFrom: <sip:sender@localhost>;tag=test\r\nTo: <sip:to@localhost>\r\nCall-ID: txms-{serial}\r\nCSeq: {serial} MESSAGE\r\nMax-Forwards: 70\r\nContent-Type: {ct}\r\nX-Inspect: {"yes" if inspect else "no"}\r\nContent-Length: {len(body)}\r\n\r\n').encode()+body
		sock.sendto(message,('127.0.0.1',port))
		response=sock.recv(65536); assert response.startswith(f'SIP/2.0 {expected}'.encode()),response
	try:
		for _ in range(50):
			if process.poll() is not None: raise RuntimeError('Kamailio exited')
			if (folder/'rpc.sock').exists(): time.sleep(.2); break
			time.sleep(.1)
		send('0XAb'); send('0xab'); send('0xGG',expected=400)
		send('0xcd\n0xef',inspect=True)
		mime='--x\r\nContent-Type: text/plain\r\nContent-Disposition: attachment; filename=a.txms.txt\r\n\r\n0x1234\r\n--x\r\nContent-Type: text/plain\r\n\r\n0x5678\r\n--x--\r\n'
		send(mime,'multipart/mixed; boundary=x')
		# UTF-16 split in the middle of an escape/code unit; assemble before decoding.
		data='~Āž'.encode('utf-16-be')
		def sms(part, seq):
			prefix=bytes([0x40,4,0x91,0x21,0x43,0,8,0x62,0x90,0x82,0x21,0,0,0])
			udh=bytes([6,8,4,0x12,42,2,seq])
			return prefix+bytes([len(udh)+len(part)])+udh+part
		send(sms(data[3:],2),'application/vnd.3gpp.sms')
		send(sms(data[:3],1),'application/vnd.3gpp.sms')
		send(sms(data[:3],1),'application/vnd.3gpp.sms')
		send('https://provider.invalid/mms','text/plain',expected=400)
		expected={'0xab','0xcd','0xef','0x1234','0x5678','0x7e'}
		for _ in range(100):
			if set(requests)==expected: break
			time.sleep(.1)
		assert set(requests)==expected,requests
		assert len(requests)==len(expected),requests
		for _ in range(30):
			v=stats()
			if v and v['succeeded']==6: break
			time.sleep(.05)
		assert v and v['succeeded']==6,v
		# Leave an incomplete group and allow the timer to clean without SIP traffic.
		partial=bytearray(sms(data[:3],1)); partial[19]=43
		send(bytes(partial),'application/vnd.3gpp.sms')
		for _ in range(80):
			v=stats()
			if v and v['groups']==0 and v['jobs']==0: break
			time.sleep(.1)
		assert v and v['groups']==0 and v['jobs']==0,v
		assert not list(folder.glob('*.db*')) and not list(folder.glob('*.sqlite*'))
		# Once retention expires, a repeat transaction can be submitted again.
		send('0xab')
		for _ in range(30):
			if len(requests)==7: break
			time.sleep(.1)
		assert requests.count('0xab')==2 and len(requests)==7,requests
		print('Real SIP integration passed: 2 SIP workers, RPC stats, idle timer expiry, resubmission after expiry, no database')
	except BaseException:
		log.flush(); log.seek(0); print(log.read(),file=sys.stderr); raise
	finally:
		os.killpg(process.pid,15)
		try: process.wait(timeout=5)
		except subprocess.TimeoutExpired: os.killpg(process.pid,9); process.wait()
		log.close(); sock.close(); control.close()
http.shutdown(); http.server_close()
