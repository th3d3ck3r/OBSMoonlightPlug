"""Real mutual TLS and HTTP requests with Qt TLS plugin discovery disabled by client."""
from pathlib import Path
import http.server
import os
import ssl
import subprocess
import sys
import tempfile
import threading
import time

binary=str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='receiver-tls-') as directory:
    root=Path(directory)
    def run(*args):
        return subprocess.run([binary,*map(str,args)],capture_output=True,text=True,timeout=4)
    result=run('--write-identity',root)
    assert result.returncode==0,result.stderr
    for name in ['server','wrong']:
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-sha256','-days','1',
                        '-subj','/CN=localhost','-keyout',str(root/f'{name}.key'),'-out',str(root/f'{name}.pem')],check=True,capture_output=True)
    state={'mode':'length','requests':0,'authenticated':False}
    payload=b'<root status_code="200"><test>ok</test></root>'
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version='HTTP/1.1'
        def log_message(self,*args):pass
        def do_GET(self):
            state['requests']+=1
            assert self.path.startswith('/probe?uniqueid=') and '&uuid=' in self.path
            if isinstance(self.connection,ssl.SSLSocket):state['authenticated']=bool(self.connection.getpeercert(binary_form=True))
            if state['mode']=='stall':time.sleep(1)
            try:
                self.send_response(200)
                self.send_header('Connection','close')
                if state['mode']=='chunked':
                    self.send_header('Transfer-Encoding','chunked');self.end_headers()
                    self.wfile.write(f'{len(payload):x}\r\n'.encode()+payload+b'\r\n0\r\n\r\n')
                else:
                    self.send_header('Content-Length',str(len(payload)));self.end_headers();self.wfile.write(payload)
            except OSError:pass
            self.close_connection=True
    class Server(http.server.ThreadingHTTPServer):
        daemon_threads=True
        def handle_error(self,*args):pass
    def server(tls):
        instance=Server(('127.0.0.1',0),Handler)
        if tls:
            context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version=ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(root/'server.pem',root/'server.key')
            context.load_verify_locations(root/'client.pem')
            context.verify_flags=0;context.verify_mode=ssl.CERT_REQUIRED
            instance.socket=context.wrap_socket(instance.socket,server_side=True)
        threading.Thread(target=instance.serve_forever,daemon=True).start()
        return instance
    https=server(True);http=server(False)
    try:
        secure=f'https://localhost:{https.server_port}'
        def request(url,pin='server.pem',timeout=2000,cancel=0):return run('--request',root,url,root/pin,timeout,cancel)
        result=request(secure)
        assert result.returncode==0 and result.stdout==payload.decode(),(result.returncode,result.stdout,result.stderr)
        assert state['authenticated'],'client TLS certificate was not presented'
        count=state['requests'];result=request(secure,'wrong.pem')
        assert result.returncode!=0 and 'certificate' in result.stderr.lower(),result.stderr
        assert state['requests']==count,'HTTP request sent before host pin validation'
        result=request(secure,'missing.pem')
        assert result.returncode!=0 and 'certificate' in result.stderr.lower(),result.stderr
        state['mode']='chunked';result=request(secure)
        assert result.returncode==0 and result.stdout==payload.decode(),result.stderr
        result=request(f'http://127.0.0.1:{http.server_port}','missing.pem')
        assert result.returncode==0 and result.stdout==payload.decode(),result.stderr
        state['mode']='stall';started=time.monotonic();result=request(secure,timeout=80)
        assert result.returncode!=0 and 'timed out' in result.stderr and time.monotonic()-started<1,result.stderr
        started=time.monotonic();result=request(secure,cancel=60)
        assert result.returncode!=0 and 'cancelled' in result.stderr and time.monotonic()-started<1,result.stderr
        print('Mutual TLS, exact host pin rejection, HTTP/chunked framing, cancellation and timeouts passed without Qt TLS plugins')
    finally:
        https.shutdown();http.shutdown();https.server_close();http.server_close()
