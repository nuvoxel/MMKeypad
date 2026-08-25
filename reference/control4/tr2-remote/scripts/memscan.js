const fs=require('fs');
const pid=parseInt(process.argv[2]);
const sigs=process.argv.slice(3);
const maps=fs.readFileSync('/proc/'+pid+'/maps','utf8').split('\n');
const fd=fs.openSync('/proc/'+pid+'/mem','r');
let hits=0, scanned=0;
const CHUNK=1<<20;
for(const line of maps){
  const m=line.match(/^([0-9a-f]+)-([0-9a-f]+) (..)./); if(!m) continue;
  if(m[3][0]!=='r') continue;                 // readable only
  let start=parseInt(m[1],16), end=parseInt(m[2],16);
  for(let off=start; off<end && hits<40; off+=CHUNK){
    const len=Math.min(CHUNK, end-off);
    const buf=Buffer.alloc(len);
    let n=0; try{ n=fs.readSync(fd,buf,0,len,off);}catch(e){continue;}
    if(n<=0) continue; scanned+=n;
    const s=buf.toString('latin1',0,n);
    for(const sig of sigs){
      let i=s.indexOf(sig);
      while(i>=0 && hits<40){
        const ctx=s.slice(Math.max(0,i-60), i+140).replace(/[^\x09\x20-\x7e]/g,'.');
        console.log('@0x'+(off+i).toString(16)+' ['+sig+'] '+ctx);
        hits++; i=s.indexOf(sig,i+1);
      }
    }
  }
}
fs.closeSync(fd);
console.error('scanned='+(scanned/1048576|0)+'MB hits='+hits);
