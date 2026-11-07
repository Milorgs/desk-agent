"""Linux MCP end-to-end tests using local fixtures and disposable settings only."""
import errno, fcntl, json, os, pty, re, select, signal, struct, tempfile, termios, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
os.chdir(Path(__file__).resolve().parent.parent)
state={'sessions':0,'deletes':0,'tools':0,'models':0,'pings':0,'errors':[]}
arguments={'value':{'count':7,'enabled':True,'items':['hello',2]}}
class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def answer(self,status,body=None,headers=None):
        data=b'' if body is None else json.dumps(body).encode()
        self.send_response(status)
        self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(data)))
        for k,v in (headers or {}).items(): self.send_header(k,v)
        self.end_headers();self.wfile.write(data)
    def do_DELETE(self):
        assert self.headers.get('Authorization')=='Bearer mcp-test-secret'
        assert self.headers.get('Mcp-Session-Id','').startswith('session-')
        state['deletes']+=1;self.answer(200,{})
    def do_POST(self):
        try: self.post()
        except Exception as e:
            state['errors'].append(repr(e));self.answer(500,{'error':'fixture assertion failed'})
    def post(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if self.path=='/mcp':
            assert self.headers.get('Authorization')=='Bearer mcp-test-secret'
            method=body.get('method')
            if method!='initialize':
                assert self.headers.get('Mcp-Session-Id','').startswith('session-')
                assert self.headers.get('MCP-Protocol-Version')=='2025-06-18'
            if method=='initialize':
                assert body['params']['capabilities']=={}
                state['sessions']+=1
                self.answer(200,{'jsonrpc':'2.0','id':body['id'],'result':{'protocolVersion':'2025-06-18','capabilities':{'tools':{}},'serverInfo':{'name':'fixture','version':'1'},'instructions':'NEVER_INSERT_THIS_SYSTEM_PROMPT'}},{'Mcp-Session-Id':f"session-{state['sessions']}"})
            elif method=='notifications/initialized': self.answer(202)
            elif method=='tools/list':
                second=body.get('params',{}).get('cursor')=='page2'
                tool={'name':'other' if second else 'echo','description':'Test nested parameters','inputSchema':{'type':'object','properties':{'value':{'type':'object'}},'required':['value']}}
                result={'tools':[tool]}
                if second: self.answer(200,{'jsonrpc':'2.0','id':body['id'],'result':result})
                else:
                    result['nextCursor']='page2'
                    self.send_response(200);self.send_header('Content-Type','text/event-stream');self.end_headers()
                    messages=[{'jsonrpc':'2.0','method':'notifications/message','params':{'data':'notice'}},{'jsonrpc':'2.0','id':'server-ping','method':'ping'},{'jsonrpc':'2.0','id':body['id'],'result':result}]
                    for msg in messages:
                        self.wfile.write(('event: message\r\ndata: '+json.dumps(msg)+'\r\n\r\n').encode());self.wfile.flush()
            elif method=='tools/call':
                state['tools']+=1;assert body['params']['name']=='echo';assert body['params']['arguments']==arguments
                self.answer(200,{'jsonrpc':'2.0','id':body['id'],'result':{'content':[{'type':'text','text':'REMOTE_MCP_OK'}],'structuredContent':arguments,'isError':False}})
            elif body.get('id')=='server-ping':
                assert body.get('result')=={};state['pings']+=1;self.answer(202)
            else: raise AssertionError(body)
        else:
            state['models']+=1
            text=json.dumps(body);assert 'mcp-test-secret' not in text and 'NEVER_INSERT_THIS_SYSTEM_PROMPT' not in text
            native=self.path.endswith('/messages')
            assert self.headers.get('x-api-key' if native else 'Authorization')==('model-test-secret' if native else 'Bearer model-test-secret')
            tools=body['tools'];defs=tools if native else [tool['function'] for tool in tools]
            external=[tool for tool in defs if ' / echo.' in tool.get('description','')];assert len(external)==1
            alias=external[0]['name'];assert alias.startswith('mcp_') and len(alias)<=64
            assert (external[0]['input_schema'] if native else external[0]['parameters'])['properties']['value']['type']=='object'
            assert 'system' not in body
            has_result='REMOTE_MCP_OK' in text
            if native:
                content=[{'type':'text','text':'MODEL_MCP_DONE'}] if has_result else [{'type':'tool_use','id':'mcp-call','name':alias,'input':arguments}]
                self.answer(200,{'stop_reason':'end_turn' if has_result else 'tool_use','content':content})
            else:
                msg={'role':'assistant','content':'MODEL_MCP_DONE'} if has_result else {'role':'assistant','content':None,'tool_calls':[{'id':'mcp-call','type':'function','function':{'name':alias,'arguments':json.dumps(arguments)}}]}
                self.answer(200,{'choices':[{'message':msg,'finish_reason':'stop' if has_result else 'tool_calls'}]})
server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
threading.Thread(target=server.serve_forever,daemon=True).start()
for native,source in ((False,'file'),(True,'url'),(False,'paste')):
    with tempfile.TemporaryDirectory() as root:
        config=Path(root)/'import.json';config.write_text(json.dumps({'mcpServers':{'remote':{'url':f'http://127.0.0.1:{server.server_port}/mcp','headers':{'Authorization':'Bearer mcp-test-secret'}}}}))
        pid,fd=pty.fork()
        if pid==0:
            fcntl.ioctl(0,termios.TIOCSWINSZ,struct.pack('HHHH',24,100,0,0))
            env={k:v for k,v in os.environ.items() if not k.startswith('DESK_')}
            env.update(TERM='xterm-256color',INPUTRC='/dev/null',XDG_CONFIG_HOME=root+'/config',XDG_STATE_HOME=root+'/state',DESK_BASE_URL=f'http://127.0.0.1:{server.server_port}/v1',DESK_MODEL='test',DESK_API_KEY='model-test-secret',DESK_API_TYPE='anthropic' if native else 'openai')
            os.execve('./build/desk',['desk','--yes','--lang','en','--workspace',root],env)
        output=bytearray();page=bytearray()
        def read():
            if select.select([fd],[],[],.1)[0]:
                try: data=os.read(fd,8192)
                except OSError as e:
                    if e.errno!=errno.EIO: raise
                    return False
                page.extend(data);output.extend(data);return bool(data)
            return True
        def until(text):
            deadline=time.monotonic()+10
            while text.encode() not in re.sub(rb'\x1b\[[0-?]*[ -/]*[@-~]',b'',page):
                assert time.monotonic()<deadline,(text,page[-4000:].decode(errors='replace'),state)
                assert read(),(text,page[-4000:].decode(errors='replace'),state)
        def send(value): page.clear();os.write(fd,value.encode())
        try:
            until('❯ 1. Start chat')
            if native:
                send('4\n')
            else:
                send('\x1b[B'*3);until('❯ 4. MCP servers')
                assert b'Select a number:' not in page
                send('\n')
            until('2. Add remote MCP URL')
            if source=='url':
                send('\x1b[B');until('❯ 2. Add remote MCP URL');send('\n');until('Server name');send('remote\n');until('MCP URL');send(f'http://127.0.0.1:{server.server_port}/mcp\n');until('Bearer token');send('mcp-test-secret\n')
            elif source=='paste':
                send('4\n');until('Save path:')
                sample=Path(root)/'MCP example.json'
                send(str(sample)+'\n');until('Example saved.')
                assert json.loads(sample.read_text())['mcpServers']['my-server']['url']=='https://example.com/mcp'
                send('3\n');until('JSON › ')
                send('\x1b[200~'+json.dumps(json.loads(config.read_text()),indent=2)+'\x1b[201~\n')
            else:
                send('\n');until('Configuration file path');send('"'+str(config)+'"\n')
            until('MCP configuration imported.');until('1. remote')
            import_end=len(output)
            send('1\n1\n');until('Connected. 2 tools available.')
            send('0\n0\n');until('❯ 1. Start chat')
            send('1\n1\nUse the MCP echo tool\n');until('MODEL_MCP_DONE')
            send('/');until('❯ 1. New conversation')
            send('\x1b[B'*5);until('❯ 6. Approval mode')
            send('\n');until('❯ 1. Ask for approval')
            send('0\n');until('❯ ')
            send('\x04');until('0. Main menu');send('0\n0\n')
            deadline=time.monotonic()+8
            while read(): assert time.monotonic()<deadline
            _,status=os.waitpid(pid,0);pid=0;assert os.waitstatus_to_exitcode(status)==0
            # Pasted JSON is echoed by the input editor. Subsequent output must not expose it.
            assert b'mcp-test-secret' not in (output[import_end:] if source=='paste' else output)
            print('PASS:', 'Anthropic / URL entry' if native else 'OpenAI / '+source, 'MCP discovery, SSE ping, pagination, session auth, nested tool call, result, cleanup')
        finally:
            if pid: os.kill(pid,signal.SIGKILL);os.waitpid(pid,0)
            os.close(fd)
assert not state['errors'],state
assert state['sessions']==6 and state['deletes']==6 and state['tools']==3 and state['models']==6 and state['pings']==6,state
server.shutdown();print('PASS:',state)
