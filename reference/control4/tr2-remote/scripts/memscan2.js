// Extract complete JSON protocol messages + Lua-source chunks from director memory.
const fs=require('fs');
const pid=parseInt(process.argv[2]);
const maps=fs.readFileSync('/proc/'+pid+'/maps','utf8').split('\n');
const fd=fs.openSync('/proc/'+pid+'/mem','r');
const CHUNK=1<<20;
const seenMsg=new Set(), seenLua=new Set();
const msgs=[], luas=[];
function extractJSON(s, anchor){         // balance braces around an anchor index
  let i=anchor; while(i>0 && s[i]!=='{') i--; if(s[i]!=='{') return null;
  let depth=0, j=i, max=i+4000;
  for(; j<s.length && j<max; j++){ if(s[j]==='{')depth++; else if(s[j]==='}'){depth--; if(depth===0){ return s.slice(i,j+1);} } }
  return null;
}
for(const line of maps){
  const m=line.match(/^([0-9a-f]+)-([0-9a-f]+) (..)./); if(!m||m[3][0]!=='r') continue;
  let start=parseInt(m[1],16), end=parseInt(m[2],16);
  for(let off=start; off<end; off+=CHUNK){
    const len=Math.min(CHUNK,end-off); const buf=Buffer.alloc(len); let n=0;
    try{n=fs.readSync(fd,buf,0,len,off);}catch(e){continue;} if(n<=0)continue;
    const s=buf.toString('latin1',0,n);
    // protocol JSON: any object mentioning remoteId or messageId
    let re=/"(remoteId|messageId|command|reply)"/g, mm;
    while((mm=re.exec(s))){ const j=extractJSON(s,mm.index);
      if(j && /"(remoteId|messageId)"/.test(j) && j.length<3500){ const k=j.replace(/\s+/g,'').slice(0,300);
        if(!seenMsg.has(k)){ seenMsg.add(k); msgs.push(j.replace(/[^\x09\x20-\x7e]/g,'.')); } } }
    // Lua source chunks from control4_remote_hub (heuristic signatures)
    for(const sig of ['AddRemote','RemoteManager','GetRemotes','registerRemote','managementDriver']){
      let i=s.indexOf(sig);
      while(i>=0){ const ctx=s.slice(Math.max(0,i-200), i+400);
        if(/function|local |C4:|end\b/.test(ctx)){ const k=ctx.slice(0,120);
          if(!seenLua.has(k)){ seenLua.add(k); luas.push('['+sig+'] '+ctx.replace(/[^\x09\x20-\x7e]/g,'.')); } }
        i=s.indexOf(sig,i+1); }
    }
  }
}
fs.closeSync(fd);
fs.writeFileSync('/tmp/proto_msgs.txt', msgs.join('\n\n'));
fs.writeFileSync('/tmp/proto_lua.txt', luas.join('\n\n'));
console.error('messages='+msgs.length+' luaChunks='+luas.length);
