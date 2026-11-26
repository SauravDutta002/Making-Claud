"""
preview.py - see every keychain screen in your browser, animated, before you flash.

    python tools/preview.py            # opens http://localhost:47900

Shows your animations from frames.h (in the order the keychain plays them)
and the built-in night ride. Run make_frames.py again, reload the page, and
the new pictures are there. Ctrl+C to stop.
"""
import http.server
import json
import os
import re
import sys
import threading
import webbrowser

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
PORT = 47900


def read_sketch():
    """Everything the page needs from frames.h and the settings in keychain.ino."""
    with open(os.path.join(ROOT, "frames.h"), encoding="utf-8") as f:
        h = f.read()
    anims = [{"name": n, "first": int(a), "count": int(c)}
             for n, a, c in re.findall(r'\{\s*"([^"]*)",\s*(\d+),\s*(\d+)\s*\}', h)]
    count = int(re.search(r"#define KC_ANIM_COUNT (\d+)", h).group(1))
    anims = anims[:count]
    m = re.search(r"KC_DELAY\[\][^{]*\{([^}]*)\}", h)
    delays = [int(x) for x in re.findall(r"\d+", m.group(1))] if m else []
    frames = []
    fm = re.search(r"KC_FRAMES\[\]\[1024\][^{]*\{(.*)\};", h, re.S)
    if fm and count:
        for block in re.findall(r"\{([^{}]*)\}", fm.group(1)):
            frames.append("".join(b[2:] for b in re.findall(r"0x[0-9a-fA-F]{2}", block)))
    with open(os.path.join(ROOT, "keychain.ino"), encoding="utf-8") as f:
        ino = f.read()
    cap = re.search(r'#define CAPTION\s+"([^"]*)"', ino)
    ride = re.search(r"#define BUILTIN_RIDE\s+(\d)", ino)
    player = re.search(r"#define BUILTIN_PLAYER\s+(\d)", ino)
    title = re.search(r'char\s+musicTitle\[44\]\s*=\s*"([^"]*)"', ino)
    return {"anims": anims, "delays": delays, "frames": frames,
            "caption": cap.group(1) if cap else "", "ride": not ride or ride.group(1) != "0",
            "player": bool(player and player.group(1) != "0"), "song": title.group(1) if title else "music",
            "candidates": read_candidates()}


def read_candidates():
    """The numbered options in art/candidates (made by art/make_candidates.py)."""
    folder = os.path.join(ROOT, "art", "candidates")
    try:
        with open(os.path.join(folder, "index.json"), encoding="utf-8") as f:
            index = json.load(f)
    except OSError:
        return []
    from PIL import Image, ImageSequence
    out = []
    for c in index:
        with open(os.path.join(folder, c["file"]), "rb") as fh:          # read it all, then let go of the file
            im = Image.open(__import__("io").BytesIO(fh.read()))
        frames, delays = [], []
        for fr in ImageSequence.Iterator(im):
            bw = fr.convert("L").point(lambda v: 255 if v > 127 else 0).convert("1")
            px = bw.load()
            data = bytearray()
            for y in range(64):
                for xb in range(0, 128, 8):
                    b = 0
                    for i in range(8):
                        if px[xb + i, y]:
                            b |= 0x80 >> i
                    data.append(b)
            frames.append(data.hex())
            delays.append(fr.info.get("duration", 70))
        out.append({**c, "frames": frames, "delays": delays})
    return out


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Keychain Screens</title>
<style>
:root{--bg:#ece9e2;--paper:#f8f6f1;--ink:#1b1a17;--ink2:#48453f;--muted:#7a766d;--line:#dcd7cc;--accent:#e0532c;--case:#24221f}
@media (prefers-color-scheme:dark){:root{--bg:#121110;--paper:#1a1917;--ink:#f1ede5;--ink2:#cbc5ba;--muted:#8e887d;--line:#2b2925;--accent:#ff6a42;--case:#2c2a27}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.5 "Segoe UI",system-ui,-apple-system,sans-serif}
.wrap{width:min(1180px,100% - 32px);margin:0 auto;padding:36px 0 48px}
header{display:flex;align-items:flex-end;justify-content:space-between;gap:16px;flex-wrap:wrap;margin-bottom:26px}
h1{margin:0;font-size:30px;letter-spacing:-.03em}
header p{margin:4px 0 0;color:var(--ink2)}
code{font:13px ui-monospace,Consolas,monospace;background:var(--paper);border:1px solid var(--line);padding:1px 6px;border-radius:6px}
.tools{display:flex;gap:8px;align-items:center}
button{font:inherit;font-size:14px;height:36px;padding:0 14px;border-radius:9px;border:1px solid var(--line);background:var(--paper);color:var(--ink);cursor:pointer}
button:hover{border-color:var(--muted)}
button.on{background:var(--ink);color:var(--paper);border-color:var(--ink)}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:16px}
.card{background:var(--paper);border:1px solid var(--line);border-radius:16px;padding:14px;cursor:zoom-in;transition:transform .15s,border-color .15s}
.card:hover{transform:translateY(-2px);border-color:var(--muted)}
.screen{background:var(--case);border-radius:12px;padding:12px}
canvas{display:block;width:100%;aspect-ratio:2/1;image-rendering:pixelated;background:#000;border-radius:3px}
.meta{display:flex;justify-content:space-between;align-items:baseline;margin-top:12px;gap:8px}
.meta b{font-weight:600}
.meta span{color:var(--muted);font-size:13px}
.num{font:12px ui-monospace,Consolas,monospace;color:var(--accent);margin-right:8px}
.empty{grid-column:1/-1;padding:22px;border:1px dashed var(--line-2,var(--line));border-radius:16px;color:var(--ink2)}
.sec{font-size:13px;letter-spacing:.12em;text-transform:uppercase;color:var(--muted);margin:34px 0 14px;font-weight:600}
.hint{margin:-6px 0 18px;color:var(--ink2)}
.group{margin:0 0 26px}
.group h3{margin:0 0 12px;font-size:17px}
.id{display:inline-grid;place-items:center;min-width:38px;height:26px;padding:0 8px;border-radius:8px;background:var(--ink);color:var(--paper);
  font:700 13px ui-monospace,Consolas,monospace;margin-right:10px}
.card.picked{border-color:var(--accent);box-shadow:0 0 0 2px var(--accent) inset}
.pk{height:30px;padding:0 12px;font-size:13px}
.card.picked .pk{background:var(--accent);border-color:var(--accent);color:#fff}
.pickbar{position:fixed;left:50%;bottom:16px;translate:-50% 0;display:flex;gap:10px;align-items:center;padding:10px 12px 10px 18px;
  border-radius:14px;background:var(--ink);color:var(--paper);box-shadow:0 10px 30px rgba(0,0,0,.25);z-index:4;max-width:calc(100% - 32px)}
.pickbar[hidden]{display:none}
.pickbar b{font-family:ui-monospace,Consolas,monospace}
.pickbar button{height:32px;background:transparent;color:var(--paper);border-color:color-mix(in srgb,var(--paper) 35%,transparent)}
.wrap{padding-bottom:90px}
.big{position:fixed;inset:0;background:rgba(0,0,0,.78);display:none;place-items:center;padding:24px;z-index:5;cursor:zoom-out}
.big.show{display:grid}
.big>div{width:min(900px,100%)}
.big .screen{padding:22px}
.big p{color:#eee;text-align:center;margin:14px 0 0}
</style></head><body>
<div class="wrap">
  <header>
    <div><h1>Keychain screens</h1>
      <p>What the keychain plays now, and new options to choose from. Click any screen to see it big.</p></div>
    <div class="tools"><button id="cap">Caption</button><button id="reload">Reload</button></div>
  </header>
  <h2 class="sec">On the keychain now</h2>
  <div class="grid" id="grid"></div>
  <div id="pick" hidden>
    <h2 class="sec">Pick your favourites</h2>
    <p class="hint">Tap <b>Pick</b> on the ones you want on the keychain, then send Claude the numbers in the bar below.</p>
    <div id="cands"></div>
  </div>
</div>
<div class="pickbar" id="pickbar" hidden><span>Picked: <b id="picked"></b></span><button id="copy">Copy</button><button id="clear">Clear</button></div>
<div class="big" id="big"><div><div class="screen"><canvas id="bigc" width="128" height="64"></canvas></div><p id="bigt"></p></div></div>
<script>
const W=128,H=64;
let data=null, showCap=true;

// ---- a tiny display, drawn like the board (1 bit) ----
class Oled{
  constructor(){this.b=new Uint8Array(W*H)}
  clear(){this.b.fill(0)}
  px(x,y,c=1){if(x>=0&&x<W&&y>=0&&y<H)this.b[y*W+x]=c}
  hline(x,y,w,c=1){for(let i=0;i<w;i++)this.px(x+i,y,c)}
  vline(x,y,h,c=1){for(let i=0;i<h;i++)this.px(x,y+i,c)}
  fill(x,y,w,h,c=1){for(let j=0;j<h;j++)this.hline(x,y+j,w,c)}
  inRR(px,py,x,y,w,h,r){if(px<x||py<y||px>=x+w||py>=y+h)return false;
    const cx=px<x+r?x+r:px>=x+w-r?x+w-1-r:px, cy=py<y+r?y+r:py>=y+h-r?y+h-1-r:py, dx=px-cx, dy=py-cy;
    return dx*dx+dy*dy<=r*r+r}
  fillRR(x,y,w,h,r,c=1){for(let j=y;j<y+h;j++)for(let i=x;i<x+w;i++)if(this.inRR(i,j,x,y,w,h,r))this.px(i,j,c)}
  drawRR(x,y,w,h,r,c=1){for(let j=y;j<y+h;j++)for(let i=x;i<x+w;i++)
    if(this.inRR(i,j,x,y,w,h,r)&&!this.inRR(i,j,x+1,y+1,w-2,h-2,r>0?r-1:0))this.px(i,j,c)}
  bitmap(hex){for(let p=0;p<W*H;p++){const byte=parseInt(hex.substr((p>>3)*2,2),16);if(byte&(0x80>>(p&7)))this.b[p]=1}}
  paint(ctx,img){for(let p=0;p<W*H;p++){const o=p*4,v=this.b[p]?235:0;img.data[o]=v;img.data[o+1]=v;img.data[o+2]=v+(this.b[p]?20:0);img.data[o+3]=255}
    ctx.putImageData(img,0,0)}
}

// ---- the built-in night ride: the same drawing code as keychain.ino ----
const WX0=8,WY0=5,WX1=119,WY1=41;
function rnd(n){n=n>>>0;n=((n<<13)^n)>>>0;
  n=(Math.imul(n,(Math.imul(Math.imul(n,n),15731)+789221)>>>0)+1376312589)>>>0;return (n>>>16)&255}
function drawRide(d,f){
  const px=(x,y)=>{if(x>WX0&&x<WX1&&y>WY0&&y<WY1)d.px(x,y)};
  const boxOut=(x,y,w,h,fill)=>{const x0=Math.max(x,WX0+1),x1=Math.min(x+w-1,WX1-1),y0=Math.max(y,WY0+1),y1=Math.min(y+h-1,WY1-1);
    if(x0>x1||y0>y1)return; if(fill){d.fill(x0,y0,x1-x0+1,y1-y0+1);return}
    if(y==y0)d.hline(x0,y0,x1-x0+1); if(x==x0)d.vline(x0,y0,y1-y0+1); if(x+w-1==x1)d.vline(x1,y0,y1-y0+1)};
  const sway=Math.sin(f*0.09)*0.6+(rnd(Math.floor(f/9))>236?1:0), sy=Math.round(sway);
  for(let i=0;i<26;i++){const x=WX0+2+rnd(i*3)%(WX1-WX0-4), y=WY0+2+rnd(i*3+1)%16; if((Math.floor(f/6)+i*7)%23)px(x,y+sy)}
  const mx=96,my=14+sy;
  for(let dy=-6;dy<=6;dy++)for(let dx=-6;dx<=6;dx++){const inn=dx*dx+dy*dy<=36, bite=(dx-3)*(dx-3)+(dy+2)*(dy+2)<=26; if(inn&&!bite)px(mx+dx,my+dy)}
  const far=f*0.35;
  for(let i=Math.floor(far/14);i<Math.floor(far/14)+10;i++){const x=i*14-Math.floor(far)+WX0,h=8+rnd(i)%12,w=9+rnd(i+99)%5,top=WY1-2-h+sy;
    boxOut(x,top,w,h+3,false);
    for(let r=top+3;r<WY1-3+sy;r+=3)for(let c=x+2;c<x+w-2;c+=3)if(rnd(i*31+r*7+c+Math.floor(f/90))>170)px(c,r)}
  const near=f*3.2;
  for(let i=Math.floor(near/64);i<Math.floor(near/64)+3;i++){const x=i*64-Math.floor(near)+WX0+20;
    for(let y=WY0+9;y<WY1;y++)px(x,y+sy); for(let k=0;k<6;k++)px(x-k,WY0+9+sy);
    boxOut(x-8,WY0+9+sy,4,2,true); px(x-7,WY0+12+sy); px(x-6,WY0+13+sy)}
  const road=WY1-2+sy; for(let x=WX0+1;x<WX1;x++)if(((x+Math.floor(f*4))%12)<7)px(x,road);
  d.drawRR(WX0,WY0,WX1-WX0+1,WY1-WY0+1,3); d.drawRR(WX0-2,WY0-2,WX1-WX0+5,WY1-WY0+5,4);
  d.fill(63,WY0,3,WY1-WY0); d.vline(30,0,H); d.vline(31,0,H);
  for(let s=0;s<2;s++){const x=s?78:6,w=s?44:40,y=47+sy; d.fillRR(x,y,w,20,5,0); d.drawRR(x,y,w,20,5); d.drawRR(x+3,y+3,w-6,20,3);
    for(let k=x+6;k<x+w-6;k+=2)d.px(k,y+9)}
  const g=Math.floor(f/2)%160; for(let k=0;k<4;k++)px(WX0+g-k+4,WY0+6+k*2);
}

// ---- the record player: the same drawing as keychain.ino ----
function circle(d,cx,cy,r,fill,c=1){for(let y=-r;y<=r;y++)for(let x=-r;x<=r;x++){const q=x*x+y*y;
  if(fill?q<=r*r+r:(q<=r*r+r&&q>=(r-1)*(r-1)+(r-1)))d.px(cx+x,cy+y,c)}}
function line(d,x0,y0,x1,y1,c=1){const n=Math.max(Math.abs(x1-x0),Math.abs(y1-y0),1);
  for(let i=0;i<=n;i++)d.px(Math.round(x0+(x1-x0)*i/n),Math.round(y0+(y1-y0)*i/n),c)}
function tri(d,ax,ay,bx,by,cx_,cy_){const mnx=Math.min(ax,bx,cx_),mxx=Math.max(ax,bx,cx_),mny=Math.min(ay,by,cy_),mxy=Math.max(ay,by,cy_);
  const s=(px,py,x1,y1,x2,y2)=>(px-x2)*(y1-y2)-(x1-x2)*(py-y2);
  for(let y=mny;y<=mxy;y++)for(let x=mnx;x<=mxx;x++){const d1=s(x,y,ax,ay,bx,by),d2=s(x,y,bx,by,cx_,cy_),d3=s(x,y,cx_,cy_,ax,ay);
    if(!((d1<0||d2<0||d3<0)&&(d1>0||d2>0||d3>0)))d.px(x,y)}}
function drawPlayer(d,f){
  const RX=64, ang=f*0.07; let [song,artist]=(data.song||"music").split(" - "); artist=artist||"";
  const w=song.length*6;
  if(w<=128-RX)text(d,song,RX,10);else{const span=w+30,off=Math.floor(f/3)%span;text(d,song,RX-off,10);text(d,song,RX-off+span,10)}
  if(artist.length>10)artist=artist.slice(0,8)+"..";
  text(d,artist,RX,21); d.fill(0,8,RX-1,12,0);
  const pos=Math.floor(f*0.025)%180, len=126-RX, fw=Math.floor(len*pos/180);
  for(let x=RX+fw;x<RX+len;x+=2)d.px(x,35); d.hline(RX,35,fw); d.fill(RX+fw-1,34,3,3);
  const B=[72,95,117];
  d.vline(B[0]-5,47,7); tri(d,B[0],47,B[0],53,B[0]-4,50); tri(d,B[0]+4,47,B[0]+4,53,B[0],50);
  d.fill(B[1]-3,47,2,7); d.fill(B[1]+1,47,2,7);
  tri(d,B[2]-4,47,B[2]-4,53,B[2],50); tri(d,B[2],47,B[2],53,B[2]+4,50); d.vline(B[2]+5,47,7);
  const cx=30,cy=32; circle(d,cx,cy,28,false); circle(d,cx,cy,27,false);
  for(const r of [24,20,16]){const n=Math.floor(2*Math.PI*r),sh=Math.floor(ang*r);
    for(let k=0;k<n;k++){if(Math.floor((k+sh)/3)%3==0)continue;const a=k*2*Math.PI/n;d.px(cx+Math.round(Math.cos(a)*r),cy+Math.round(Math.sin(a)*r))}}
  for(let r=13;r<=25;r++){d.px(cx+Math.floor(r*0.7),cy-Math.floor(r*0.7));d.px(cx-Math.floor(r*0.7),cy+Math.floor(r*0.7))}
  circle(d,cx,cy,10,true);
  for(let k=0;k<3;k++){const a=ang+k*Math.PI/3,dx=Math.round(Math.cos(a)*7),dy=Math.round(Math.sin(a)*7);
    line(d,cx-dx,cy-dy,cx+dx,cy+dy,0);line(d,cx-dx+1,cy-dy,cx+dx+1,cy+dy,0)}
  circle(d,cx,cy,1,true); circle(d,60,6,3,true); circle(d,60,6,1,true,0);
  line(d,60,8,55,24);line(d,61,8,56,24);line(d,55,24,47,22);d.fill(45,21,4,3);
}

// ---- screens ----
function screens(){
  const list=data.anims.map((a,i)=>({name:a.name||("animation "+(i+1)),sub:`${a.count} frame${a.count==1?"":"s"} · yours`,anim:a}));
  if(data.ride||(!list.length&&!data.player))list.push({name:"Night ride",sub:"built in"});
  if(data.player)list.push({name:"Record player",sub:"built in",player:true});
  return list;
}
function player(scr){                                  // -> draw(oled, ms) for one screen
  if(scr.cand){const c=scr.cand;let k=0,acc=0;
    return (d,dt)=>{acc+=dt;let ms=c.delays[k]||70;while(acc>=ms){acc-=ms;k=(k+1)%c.frames.length;ms=c.delays[k]||70}d.bitmap(c.frames[k])}}
  if(scr.player){let f=0,acc=0;return (d,dt)=>{acc+=dt;while(acc>=25){acc-=25;f++}drawPlayer(d,f)}}
  if(!scr.anim){let f=0,acc=0;return (d,dt)=>{acc+=dt;while(acc>=25){acc-=25;f++}drawRide(d,f)}}
  const a=scr.anim;let k=0,acc=0;
  return (d,dt)=>{acc+=dt;let ms=data.delays[a.first+k]||83;while(acc>=ms){acc-=ms;k=(k+1)%a.count;ms=data.delays[a.first+k]||83}
    const hex=data.frames[a.first+k]; if(hex)d.bitmap(hex)}
}
// the board's own 5x7 font (Adafruit GFX "classic"), printable ASCII
const FONT="000000000000005f00000007000700147f147f14242a7f2a12231308646236495620500008070300001c2241000041221c002a1c7f1c2a08083e080800807030000808080808000060600020100804023e5149453e00427f400072494949462141494d331814127f1027454545393c4a49493141211109073649494936464949291e0000140000004034000000081422411414141414004122140802015909063e415d594e7c1211127c7f494949363e414141227f4141413e7f494949417f090909013e414151737f0808087f00417f41002040413f017f081422417f404040407f021c027f7f0408107f3e4141413e7f090909063e4151215e7f09192946264949493203017f01033f4040403f1f2040201f3f4038403f631408146303047804036159494d43007f4141410204081020004141417f04020102044040404040000307080020545478407f284444383844444428384444287f385454541800087e090218a4a49c787f0804047800447d40002040403d007f1028440000417f40007c047804787c080404783844444438fc1824241818242418fc7c08040408485454542404043f44243c4040207c1c2040201c3c4030403c44281028444c9090907c4464544c440008364100000077000000413608000201020402";
function text(d,s,x,y){for(const ch of s){let c=ch.charCodeAt(0);if(c<32||c>126)c=63;
  for(let i=0;i<5;i++){const col=parseInt(FONT.substr(((c-32)*5+i)*2,2),16);for(let j=0;j<8;j++)if(col&(1<<j))d.px(x+i,y+j)}x+=6}}
function caption(d){                                   // drawCaption() in keychain.ino
  if(!showCap||!data.caption)return;
  d.fill(0,H-10,data.caption.length*6+4,10,0); text(d,data.caption,2,H-8);
}

let views=[];
function build(){
  const grid=document.getElementById("grid"); grid.innerHTML=""; views=[];
  const list=screens();
  if(!data.anims.length){const e=document.createElement("div");e.className="empty";
    e.innerHTML="No pictures of your own yet. Run <code>python tools/make_frames.py your.gif</code> and reload: they appear here, before the night ride.";grid.append(e)}
  list.forEach((s,i)=>{
    const card=document.createElement("div");card.className="card";
    card.innerHTML=`<div class="screen"><canvas width="128" height="64"></canvas></div><div class="meta"><b><span class="num">${String(i+1).padStart(2,"0")}</span>${s.name}</b><span>${s.sub}</span></div>`;
    grid.append(card);
    const c=card.querySelector("canvas"),ctx=c.getContext("2d");
    views.push({ctx,img:ctx.createImageData(W,H),play:player(s),d:new Oled()});
    card.onclick=()=>openBig(s,i);
  });
}
// ---- the options to pick from ----
let picks=[];
try{picks=JSON.parse(localStorage.getItem("kc-picks")||"[]")}catch(e){}
function savePicks(){try{localStorage.setItem("kc-picks",JSON.stringify(picks))}catch(e){}
  document.getElementById("pickbar").hidden=!picks.length;
  document.getElementById("picked").textContent=picks.join(", ");
  document.querySelectorAll("[data-id]").forEach(el=>el.classList.toggle("picked",picks.includes(el.dataset.id)))}
function buildCands(){
  const box=document.getElementById("cands");box.innerHTML="";
  const list=data.candidates||[];document.getElementById("pick").hidden=!list.length;
  const groups=[];list.forEach(c=>{let g=groups.find(g=>g.name==c.group);if(!g){g={name:c.group,items:[]};groups.push(g)}g.items.push(c)});
  for(const g of groups){
    const sec=document.createElement("div");sec.className="group";sec.innerHTML=`<h3>${g.name}</h3><div class="grid"></div>`;box.append(sec);
    const grid=sec.querySelector(".grid");
    for(const c of g.items){
      const s={name:c.group+" · "+c.look,cand:c};
      const card=document.createElement("div");card.className="card";card.dataset.id=c.id;
      card.innerHTML=`<div class="screen"><canvas width="128" height="64"></canvas></div><div class="meta"><b><span class="id">${c.id}</span>${c.look}</b><button class="pk">Pick</button></div>`;
      grid.append(card);
      const ctx=card.querySelector("canvas").getContext("2d");
      views.push({ctx,img:ctx.createImageData(W,H),play:player(s),d:new Oled(),cand:true});
      card.onclick=(e)=>{if(e.target.classList.contains("pk")){e.stopPropagation();
        picks=picks.includes(c.id)?picks.filter(x=>x!=c.id):[...picks,c.id];savePicks();return}
        openBig(s,c.id)};
    }
  }
  savePicks();
}
document.getElementById("copy").onclick=()=>{navigator.clipboard&&navigator.clipboard.writeText(picks.join(", "));
  const b=document.getElementById("copy");b.textContent="Copied";setTimeout(()=>b.textContent="Copy",1200)};
document.getElementById("clear").onclick=()=>{picks=[];savePicks()};

let big=null;
function openBig(s,i){const c=document.getElementById("bigc"),ctx=c.getContext("2d");
  big={ctx,img:ctx.createImageData(W,H),play:player(s),d:new Oled()};
  document.getElementById("bigt").textContent=typeof i=="string"?`${i} · ${s.name}`:`${String(i+1).padStart(2,"0")} · ${s.name}`;
  document.getElementById("big").classList.add("show")}
document.getElementById("big").onclick=()=>{big=null;document.getElementById("big").classList.remove("show")};
addEventListener("keydown",e=>{if(e.key==="Escape")document.getElementById("big").click()});

let last=performance.now();
function tick(t){const dt=Math.min(200,t-last);last=t;
  for(const v of big?[big]:views){v.d.clear();v.play(v.d,dt);if(!v.cand)caption(v.d);v.d.paint(v.ctx,v.img)}
  requestAnimationFrame(tick)}

async function load(){data=await (await fetch("data.json",{cache:"no-store"})).json();
  const cb=document.getElementById("cap"); cb.style.display=data.caption?"":"none"; cb.classList.toggle("on",showCap); build(); buildCands()}
document.getElementById("reload").onclick=load;
document.getElementById("cap").onclick=()=>{showCap=!showCap;document.getElementById("cap").classList.toggle("on",showCap)};
load().then(()=>requestAnimationFrame(tick));
</script></body></html>
"""


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body, ctype):
        b = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in ("/", "/index.html"):
            return self.send(200, PAGE, "text/html; charset=utf-8")
        if path == "/app" or path.startswith("/app/"):           # the phone remote (keychain/app)
            if path == "/app":
                self.send_response(301); self.send_header("Location", "/app/"); self.end_headers(); return
            name = path[len("/app/"):] or "index.html"
            full = os.path.normpath(os.path.join(ROOT, "app", name))
            if not full.startswith(os.path.join(ROOT, "app")) or not os.path.isfile(full):
                return self.send(404, "not found", "text/plain")
            types = {".html": "text/html; charset=utf-8", ".js": "text/javascript", ".svg": "image/svg+xml",
                     ".webmanifest": "application/manifest+json", ".css": "text/css", ".png": "image/png",
                     ".woff": "font/woff", ".woff2": "font/woff2", ".bin": "application/octet-stream", ".json": "application/json"}
            body = open(full, "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", types.get(os.path.splitext(full)[1], "application/octet-stream"))
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
            return
        if path == "/data.json":
            try:
                return self.send(200, json.dumps(read_sketch()), "application/json")
            except Exception as e:
                return self.send(500, json.dumps({"error": str(e)}), "application/json")
        self.send(404, "not found", "text/plain")


def main():
    try:
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    except OSError:
        sys.exit(f"port {PORT} is busy - is the preview already open? http://localhost:{PORT}")
    url = f"http://localhost:{PORT}"
    print(f"keychain preview: {url}   (Ctrl+C to stop)")
    if "--no-open" not in sys.argv:
        threading.Timer(0.4, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
