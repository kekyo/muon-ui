// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

/** Builds a page-side probe using the installed services API and each runtime's external listener. */
export const exerciseConsumerNetwork = (ports: readonly number[]) => `
  const bytes = new Uint8Array([0, 128, 255]);
  for (const [index,name] of ['native','quick','node'].entries()) {
    const listener = await api.acquire(name + 'Listener');
    let client;
    try {
      client = await api.acquire(name + 'Http');
      const url = 'http://127.0.0.1:' + ${JSON.stringify(ports)}[index] + '/echo';
      const headers = {authorization:'Bearer local-service'};
      const reply = await client.call('bytes',[JSON.stringify({url,headers,method:'POST'}),bytes]);
      const meta = JSON.parse(await client.call('describe',[JSON.stringify({url,headers,responseType:'text'}),new Uint8Array()]));
      if (Array.from(reply).join(',')!=='0,128,255' || meta.status!==200 ||
          meta.text!==listener.generation || meta.headers['x-service-generation']!==listener.generation)
        throw new Error(name+' listener '+listener.generation+': '+JSON.stringify({bytes:Array.from(reply),meta}));
    } finally { try { await client?.release(); } finally { await listener.release(); } }
  }
  return true;`;
