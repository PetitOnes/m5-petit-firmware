// Browser-based SD asset installer, served by the M5 itself at GET /assets.
// Lets the user install the bundled default assets (sd.zip fetched from GitHub
// and unzipped in the browser) or upload their own .jpg/.wav files -- no PC
// tools needed. Defaults to pushing to the M5 that served this page, but the
// target IP field can be pointed at any other Petit on the network (the
// upload/list endpoints send Access-Control-Allow-Origin: * so this works
// cross-origin too).
//
// M5自身が GET /assets で配信するアセット導入ページ。標準アセット(sd.zip)を
// ブラウザがGitHubから直接取得・展開してM5へ送るので、PC側のツールは不要。
// 自分の .jpg/.wav のアップロードにも対応。送信先IP欄を書き換えれば、この
// ページを開いたM5以外の別プチ宛てにも送れる（アップロード/一覧エンドポイントは
// Access-Control-Allow-Origin: * を返すのでクロスオリジンでも動く）。
#pragma once

static const char ASSETS_PAGE_HTML[] PROGMEM = R"HTMLPAGE(<!doctype html>
<html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>M5 Petit Assets</title><style>
:root{--turquoise:#00afcc;--yellow:#fff262;--lavender:#cab8d9;
--ok-bg:#15321f;--ok-bd:#3ddc84;--warn-bg:#3a2410;--warn-bd:#ffb86b}
body{font-family:system-ui,-apple-system,'Hiragino Sans','Noto Sans JP',sans-serif;
max-width:560px;margin:0 auto;padding:18px;line-height:1.7;background:#111;color:#eee;font-size:16px}
h1{font-size:1.3rem;margin-bottom:4px}
h1+p{opacity:0.85;margin-top:0}
h2{font-size:1.05rem;margin:0 0 8px;font-weight:700}
.card{border-radius:14px;padding:14px 16px;margin:14px 0;background:rgba(255,255,255,0.05);
border-left:5px solid var(--c,#555)}
.card.target{--c:var(--turquoise)}
.card.default{--c:var(--yellow)}
.card.own{--c:var(--lavender)}
button{font-size:1rem;padding:11px 16px;border-radius:10px;border:none;background:#4a9;color:#000;
margin-top:10px;font-weight:600}
button:disabled{opacity:0.4}
input[type=file]{margin-top:8px;width:100%}
input[type=text]{font-size:1rem;padding:9px;border-radius:8px;border:1px solid #555;
background:#222;color:#eee;width:100%;box-sizing:border-box}
.row{margin:8px 0}
#progress{font-size:0.9rem;font-weight:600;min-height:1.3em;margin-top:10px}
#log{font-size:0.8rem;white-space:pre-wrap;background:#000;border-radius:10px;
padding:10px;margin-top:8px;max-height:34vh;overflow-y:auto}
.lists{font-size:0.8rem;opacity:0.8;word-break:break-all}
.note{font-size:0.85rem;opacity:0.75}
#banner{display:none;border-radius:14px;padding:16px;margin-top:14px;font-size:1.05rem;font-weight:700}
#banner.ok{display:block;background:var(--ok-bg);border:2px solid var(--ok-bd)}
#banner.fail{display:block;background:var(--warn-bg);border:2px solid var(--warn-bd)}
#banner p{margin:8px 0 0;font-weight:400;font-size:0.9rem}
#banner button{background:var(--warn-bd);color:#000}
</style></head><body>
<h1>📦 SDアセット導入 / SD Assets</h1>
<p class="note">顔画像と効果音をSDカードにWiFi経由で入れます。SDカードが挿さっている必要があります。<br>
Installs face images &amp; sounds onto an SD card over WiFi. An SD card must be inserted.</p>

<div class="card target">
<h2>送信先 / Target</h2>
<p class="note">このページを開いたM5宛てが初期値。別のプチに送るならIPを書き換える。<br>
Defaults to the M5 that served this page. Change the IP to target a different Petit.</p>
<div class="row"><input type="text" id="targetIp" placeholder="192.168.1.109"></div>
</div>

<div class="card default">
<h2>標準アセット / Default assets</h2>
<p class="note">GitHubから標準セット（顔7種・効果音12種）を取得して送信します。<br>
Fetches the bundled set from GitHub and uploads it.</p>
<button id="btnDefault">標準アセットを入れる / Install defaults</button>
</div>

<div class="card own">
<h2>自分のファイル / Your own files</h2>
<p class="note">.jpg は顔画像、.wav は効果音（16bit PCM mono 16kHz）として保存されます。<br>
.jpg files go to face images, .wav files to sounds.</p>
<input type="file" id="filePick" multiple accept=".jpg,.wav">
<button id="btnUpload">選んだファイルを送信 / Upload selected</button>
</div>

<div id="progress"></div>
<div id="banner"></div>

<h2>現在のSDの中身 / Currently on SD</h2>
<div class="lists" id="lists">(loading...)</div>
<div id="log"></div>
<script>
const ZIP_URL='https://raw.githubusercontent.com/PetitOnes/m5-petit-firmware/main/sd.zip';
const log=(m)=>{const el=document.getElementById('log');el.textContent+=m+'\n';el.scrollTop=el.scrollHeight;};
const targetBase=()=>{
  const v=document.getElementById('targetIp').value.trim();
  return v?('http://'+v):'';  // 空なら同一オリジン(相対パス) / empty = same-origin (relative)
};
document.getElementById('targetIp').placeholder=location.hostname;
async function refreshLists(){
  try{
    const base=targetBase();
    const f=await (await fetch(base+'/face_list')).json();
    const s=await (await fetch(base+'/se_list')).json();
    document.getElementById('lists').textContent='face: '+f.join(', ')+'\nwav: '+s.join(', ');
  }catch(e){document.getElementById('lists').textContent='(取得失敗 / failed to load: '+e.message+')';}
}
async function unzip(buf){
  const dv=new DataView(buf),u8=new Uint8Array(buf);
  let eocd=-1;
  for(let i=buf.byteLength-22;i>=Math.max(0,buf.byteLength-22-65536);i--){
    if(dv.getUint32(i,true)===0x06054b50){eocd=i;break;}
  }
  if(eocd<0)throw new Error('zip: EOCD not found');
  const count=dv.getUint16(eocd+10,true);
  let off=dv.getUint32(eocd+16,true);
  const out=[];
  for(let n=0;n<count;n++){
    if(dv.getUint32(off,true)!==0x02014b50)throw new Error('zip: bad central directory');
    const method=dv.getUint16(off+10,true);
    const csize=dv.getUint32(off+20,true);
    const nlen=dv.getUint16(off+28,true);
    const elen=dv.getUint16(off+30,true);
    const clen=dv.getUint16(off+32,true);
    const lho=dv.getUint32(off+42,true);
    const name=new TextDecoder().decode(u8.subarray(off+46,off+46+nlen));
    const lnlen=dv.getUint16(lho+26,true),lelen=dv.getUint16(lho+28,true);
    const comp=buf.slice(lho+30+lnlen+lelen,lho+30+lnlen+lelen+csize);
    if(!name.endsWith('/')){
      let data;
      if(method===0)data=comp;
      else if(method===8)data=await new Response(new Blob([comp]).stream()
        .pipeThrough(new DecompressionStream('deflate-raw'))).arrayBuffer();
      else{off+=46+nlen+elen+clen;continue;}
      out.push({name,data});
    }
    off+=46+nlen+elen+clen;
  }
  return out;
}
function setProgress(cur,total){
  document.getElementById('progress').textContent=
    cur<=total?('⏳ '+cur+'/'+total+' 送信中... / uploading...'):'';
}
function showBanner(fail,total,onRetry){
  const el=document.getElementById('banner');
  if(total===0){el.className='';el.innerHTML='';return;}
  if(fail===0){
    el.className='ok';
    el.innerHTML='✅ 完了！すべて送信できました（'+total+'件）/ All done! ('+total+' files)';
  }else{
    el.className='fail';
    el.innerHTML='⚠️ '+fail+'件失敗しました。WiFiが不安定かもしれません。<b>もう一回押してください</b>'+
      '（送信済みの分は上書きされるだけなので何度でも安全です）<br>'+
      fail+' failed — likely flaky WiFi. <b>Tap retry</b>; already-sent files are safely overwritten.'+
      '<p><button id="btnRetry">もう一回送る / Retry</button></p>';
    document.getElementById('btnRetry').onclick=onRetry;
  }
  el.scrollIntoView({behavior:'smooth',block:'center'});
}
async function uploadOnce(name,data){
  const base=name.split('/').pop(),lower=base.toLowerCase();
  const ep=lower.endsWith('.jpg')?'/upload_face':lower.endsWith('.wav')?'/upload_wav':null;
  if(!ep){log('  skip: '+name);return true;}
  const fd=new FormData();
  fd.append('file',new Blob([data]),base);
  try{
    const r=await fetch(targetBase()+ep,{method:'POST',body:fd});
    log((r.ok?'  ok: ':'  FAILED: ')+base);
    return r.ok;
  }catch(e){log('  FAILED: '+base+' ('+e.message+')');return false;}
}
async function uploadWithAutoRetry(item){
  if(await uploadOnce(item.name,await item.getData()))return true;
  log('  retrying once: '+item.name.split('/').pop());
  await new Promise(r=>setTimeout(r,500));
  return await uploadOnce(item.name,await item.getData());
}
async function runBatch(items){
  const total=items.length,failed=[];
  for(let i=0;i<items.length;i++){
    setProgress(i+1,total);
    if(!await uploadWithAutoRetry(items[i]))failed.push(items[i]);
  }
  setProgress(total+1,total);
  showBanner(failed.length,total,()=>withButtons(()=>runBatch(failed)));
  return failed.length;
}
async function withButtons(fn){
  const btns=[document.getElementById('btnDefault'),document.getElementById('btnUpload')];
  btns.forEach(b=>b.disabled=true);
  try{await fn();}catch(e){log('error: '+e.message);}
  btns.forEach(b=>b.disabled=false);
  refreshLists();
}
document.getElementById('btnDefault').onclick=()=>withButtons(async()=>{
  document.getElementById('banner').className='';
  log('GitHubから取得中... / fetching sd.zip ...');
  const buf=await (await fetch(ZIP_URL)).arrayBuffer();
  log('展開中... / unzipping ('+buf.byteLength+' bytes)');
  const entries=await unzip(buf);
  const items=entries.map(e=>({name:e.name,getData:()=>Promise.resolve(e.data)}));
  await runBatch(items);
});
document.getElementById('btnUpload').onclick=()=>withButtons(async()=>{
  document.getElementById('banner').className='';
  const files=document.getElementById('filePick').files;
  if(!files.length){log('ファイルが選ばれていません / no files selected');return;}
  const items=Array.from(files).map(f=>({name:f.name,getData:()=>f.arrayBuffer()}));
  await runBatch(items);
});
refreshLists();
</script></body></html>
)HTMLPAGE";
