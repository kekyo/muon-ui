// An external client exercises the service listener independently of each UI connection.
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { request } from 'node:http';
import { createServer, type Socket } from 'node:net';
import { join } from 'node:path';
import type { createMuonServicesApi } from '../common/renderer/services.ts';

type Page = {api:ReturnType<typeof createMuonServicesApi>;socket:Socket};
export const createServerDriver = async (assets: string,binaries: string) => {
  const ids = ['listenNative','listenQuick','listenNode'];
  const ports: number[] = [];
  const reservations = ids.map(()=>createServer());
  try {
    for (const server of reservations) {
      server.listen(0,'127.0.0.1'); await once(server,'listening');
      const address = server.address(); assert(address && typeof address !== 'string'); ports.push(address.port);
    }
  } finally { for (const server of reservations) { const closed = once(server,'close'); server.close(); await closed; } }
  const sources = [
    {library:join(binaries,process.platform==='win32'?'server-service.dll':'server-service.so')},
    {runtime:'quickjs',script:join(assets,'server-quick.js')},
    {runtime:'node',project:assets,entry:'./server-node.mjs',executable:process.execPath,bridge:join(assets,'node-bridge.mjs')},
  ];
  const auth = {Authorization:'Bearer local-service'};
  const query = async (port:number, headers: Record<string,string>, data?: Uint8Array) => await new Promise<{status:number;generation:unknown;body:Buffer}>((resolve,reject)=>{
    const req=request({hostname:'127.0.0.1',port,path:'/echo',method:data?'POST':'GET',headers,agent:false},response=>{
      const chunks:Buffer[]=[];response.on('data',chunk=>chunks.push(chunk));response.on('error',reject);
      response.on('end',()=>resolve({status:response.statusCode!,generation:response.headers['x-service-generation'],body:Buffer.concat(chunks)}));
    });req.on('error',reject);req.end(data);
  });
  return {
    services:Object.fromEntries(ids.map((id,i)=>[id,{...sources[i],io:{server:{listen:true}},config:{port:String(ports[i]),bearerToken:"local-service"}}])),
    verify:async (connect:()=>Promise<Page>)=>{
      const control=await connect();
      for(const [index,id] of ids.entries()) {
        const otherIndex=(index+1)%ids.length;
        const other=await control.api.acquire(ids[otherIndex]!);
        await control.api.setAutostart(id,true);
        const page=await connect();
        const handle=await page.api.acquire(id);
        const generation=handle.generation;
        const closedPage=once(page.socket,'close');page.socket.destroy();await closedPage;
        const port=ports[index]!;
        assert.equal((await query(port,{})).status,401);
        assert.equal((await query(port,{...auth,Origin:'https://unlisted.test'})).status,403);
        assert.notEqual((await query(port,{...auth,Host:'unlisted.test'})).status,200);
        const bytes=Uint8Array.from([0,128,255]);
        const res=await query(port,{...auth,Origin:'https://example.test'},bytes);
        assert.equal(res.status,200);assert.equal(res.generation,generation);assert.deepEqual(res.body,Buffer.from(bytes));
        const ws=new WebSocket('ws://127.0.0.1:'+port+'/echo',['muon.auth','muon.bearer.'+Buffer.from('local-service').toString('base64url')]);
        ws.binaryType='arraybuffer';
        await once(ws,'open');
        for(const message of ['茶',bytes]) {
          const delivered=once(ws,'message'); ws.send(message);
          const [event]=await delivered;
          if(typeof message==='string')assert.equal(event.data,message);else assert.deepEqual(new Uint8Array(event.data),message);
        }
        const disconnected=once(ws,'close');
        const last=await control.api.acquire(id);assert.equal(last.generation,generation);
        await control.api.setAutostart(id,false);
        await last.release();await disconnected;
        await assert.rejects(query(port,auth),{code:'ECONNREFUSED'});
        assert.equal((await query(ports[otherIndex]!,auth)).generation,other.generation);
        const next=await control.api.acquire(id);assert.notEqual(next.generation,generation);
        assert.equal((await query(port,auth)).generation,next.generation);
        await next.release();await other.release();
      }
      control.socket.destroy();
      console.log(JSON.stringify({serviceListeners:true,passed:true}));
    },
  };
};
