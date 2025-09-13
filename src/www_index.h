#pragma once
#include <Arduino.h>

// Edit this file to change the UI
extern const char INDEX_HTML[] PROGMEM;

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>NozzleCAM</title>
<style>
  :root,html,body{height:100%;margin:0}
  body{background:#000;color:#fff;font-family:system-ui,Arial,sans-serif}
  .bar{
    position:fixed;left:0;right:0;top:0;z-index:10;
    display:flex;gap:.5rem;align-items:center;justify-content:space-between;
    padding:.5rem .75rem;background:rgba(0,0,0,.4);backdrop-filter:blur(6px)
  }
  .left,.right{display:flex;gap:.5rem;align-items:center}
  .brand{color:#fff;text-decoration:none;font-weight:600}

  /* Base icon buttons */
  button.icon, a.icon{
    width:42px;height:42px;padding:0;display:inline-block;
    border:1px solid #333;border-radius:.6rem;background:#111 center/24px 24px no-repeat;
    cursor:pointer;outline:none;text-decoration:none
  }
  button.icon:focus-visible, a.icon:focus-visible{box-shadow:0 0 0 2px #09f6}
  button.icon:hover, a.icon:hover{background-color:#141414}
  button.icon:active{transform:translateY(1px)}
  button.icon.toggle.on{box-shadow:inset 0 0 0 2px #0af}
  button.icon{background-image:var(--img)}
  a.icon{background-image:var(--img)}

  /* Snap / Record / Fullscreen icons */
  #shot{--img:url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path fill='%23fff' d='M9 4l1.5 2H18a2 2 0 012 2v8a2 2 0 01-2 2H6a2 2 0 01-2-2V8a2 2 0 012-2h2.5L9 4zm3 4a5 5 0 100 10 5 5 0 000-10zm0 2a3 3 0 110 6 3 3 0 010-6z'/></svg>")}
  #rec{--img:url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><circle cx='12' cy='12' r='6' fill='%23e53935'/></svg>")}
  #rec.on{--img:url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><rect x='7' y='7' width='10' height='10' rx='2' fill='%23e53935'/></svg>")}
  #fs{--img:url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path fill='%23fff' d='M4 9V4h5v2H6v3H4zm10-5h5v5h-2V6h-3V4zM4 15h2v3h3v2H4v-5zm13 3v-3h2v5h-5v-2h3z'/></svg>")}
  #fs.on{--img:url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path fill='%23fff' d='M9 7V4H4v5h2V7h3zm9 2h2V4h-5v3h3v2zM7 15H4v5h5v-2H7v-3zm10 3h-3v2h5v-5h-2v3z'/></svg>")}

  /* Settings gear: a bit smaller and at far right */
  #settings{
    --img:url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path fill='%23fff' d='M19.14 12.94a7.973 7.973 0 000-1.88l2.03-1.58a.5.5 0 00.12-.64l-1.92-3.32a.5.5 0 00-.6-.22l-2.39.96a7.98 7.98 0 00-1.63-.95l-.36-2.54A.5.5 0 0013.9 0h-3.8a.5.5 0 00-.5.42l-.36 2.54c-.57.23-1.11.54-1.63.95l-2.39-.96a.5.5 0 00-.6.22L.84 6.04a.5.5 0 00.12.64l2.03 1.58c-.05.31-.09.62-.09.94s.04.63.09.94L.96 11.72a.5.5 0 00-.12.64l1.92 3.32c.14.24.43.34.68.22l2.39-.96c.51.41 1.06.73 1.63.95l.36 2.54c.04.25.25.42.5.42h3.8c.25 0 .46-.17.5-.42l.36-2.54c.57-.23 1.11-.54 1.63-.95l2.39.96c.25.1.54 0 .68-.22l1.92-3.32a.5.5 0 00-.12-.64l-2.03-1.58zM12 15.5A3.5 3.5 0 1112 8a3.5 3.5 0 010 7.5z'/></svg>");
    width:34px;height:34px;           /* smaller footprint */
    background-size:20px 20px;        /* smaller glyph */
    border-radius:.6rem;border:1px solid #333;background-color:#111;
    margin-left:.25rem;                /* slight spacing from neighbors */
  }

  #dl{display:none}
  #stage{position:fixed;inset:0;display:flex;align-items:center;justify-content:center}
  #stream{display:block;width:100vw;height:100vh;object-fit:contain;background:#000;touch-action:none}
  canvas{display:none}
</style>
</head><body>
  <div class="bar">
    <div class="left">
      <a class="brand" href="/">NozzleCAM</a>
    </div>
    <div class="right">
      <a id="dl" class="btn" download>Save file…</a>
      <button id="shot" class="icon" aria-label="Snapshot" title="Snapshot"></button>
      <button id="rec" class="icon toggle" aria-label="Record" title="Record" aria-pressed="false"></button>
      <button id="fs"  class="icon toggle" aria-label="Fullscreen" title="Fullscreen" aria-pressed="false"></button>
      <!-- Settings goes LAST so it sits at the far right edge -->
      <a id="settings" class="icon" href="/settings" aria-label="Settings" title="Settings"></a>
    </div>
  </div>

  <div id="stage">
    <img id="stream" alt="Live stream">
    <canvas id="cvs"></canvas>
  </div>

<script>
  const img=document.getElementById('stream');
  const cvs=document.getElementById('cvs');
  const ctx=cvs.getContext('2d');
  const dl=document.getElementById('dl');
  const btnShot=document.getElementById('shot');
  const btnRec=document.getElementById('rec');
  const btnFS=document.getElementById('fs');

  img.src='/stream';

  function syncFSButton(){
    const on=!!document.fullscreenElement;
    btnFS.classList.toggle('on',on);
    btnFS.setAttribute('aria-pressed',on?'true':'false');
  }
  btnFS.onclick=()=>{const el=document.documentElement; if(document.fullscreenElement) document.exitFullscreen(); else if(el.requestFullscreen) el.requestFullscreen();};
  document.addEventListener('fullscreenchange',syncFSButton);

  function syncCanvasToImage(){
    const w=img.naturalWidth||img.videoWidth||img.width;
    const h=img.naturalHeight||img.videoHeight||img.height;
    if(w&&h&&(cvs.width!==w||cvs.height!==h)){cvs.width=w;cvs.height=h;}
  }

  let lastURL=null;
  function showFallbackLink(url,filename){
    if(lastURL&&lastURL!==url){try{URL.revokeObjectURL(lastURL);}catch(e){}}
    lastURL=url; dl.href=url; dl.download=filename; dl.style.display='inline-block';
    showMsg('Tap "Save file…" to store locally');
  }

  async function saveBlobSmart(blob,filename,mime){
    const file=new File([blob],filename,{type:mime});
    try{
      if(navigator.canShare&&navigator.canShare({files:[file]})){
        await navigator.share({files:[file],title:'NozzleCAM'}); showMsg('Shared'); return;
      }
    }catch(e){}
    const url=URL.createObjectURL(blob);
    try{
      const a=document.createElement('a'); a.href=url; a.download=filename;
      document.body.appendChild(a); a.click(); a.remove();
      showMsg('Saved to Downloads'); setTimeout(()=>URL.revokeObjectURL(url),3000);
    } catch(e){ showFallbackLink(url,filename); }
  }

  btnShot.onclick=async()=>{
    try{
      syncCanvasToImage();
      if(!cvs.width||!cvs.height){showMsg('No frame yet');return;}
      ctx.drawImage(img,0,0,cvs.width,cvs.height);
      cvs.toBlob(async(blob)=>{
        if(!blob){showMsg('Snapshot failed');return;}
        const ts=new Date().toISOString().replace(/[:.]/g,'-');
        await saveBlobSmart(blob,`NozzleCAM_${ts}.jpg`,'image/jpeg');
      },'image/jpeg',0.95);
    }catch(e){showMsg('Snapshot failed');}
  };

  let rec=null,chunks=[],drawTimer=null;
  function setRecUI(on){btnRec.classList.toggle('on',on);btnRec.setAttribute('aria-pressed',on?'true':'false');}
  btnRec.onclick=()=>{
    if(rec&&rec.state!=='inactive'){clearInterval(drawTimer);drawTimer=null;rec.stop();return;}
    if(typeof MediaRecorder==='undefined'){showMsg('Recording not supported');return;}
    syncCanvasToImage();
    if(!cvs.width||!cvs.height){showMsg('No frame yet');return;}
    const fps=20;
    drawTimer=setInterval(()=>{
      try{
        if(!img.complete)return;
        if(img.naturalWidth&&(img.naturalWidth!==cvs.width||img.naturalHeight!==cvs.height)){
          cvs.width=img.naturalWidth; cvs.height=img.naturalHeight;
        }
        ctx.drawImage(img,0,0,cvs.width,cvs.height);
      }catch(e){}
    },Math.round(1000/fps));
    const stream=cvs.captureStream(fps);
    chunks=[];
    let mime='video/webm;codecs=vp9';
    if(!MediaRecorder.isTypeSupported(mime)) mime='video/webm;codecs=vp8';
    if(!MediaRecorder.isTypeSupported(mime)) mime='video/webm';
    try{rec=new MediaRecorder(stream,{mimeType:mime,videoBitsPerSecond:5_000_000});}
    catch(e){showMsg('Recording not supported');clearInterval(drawTimer);return;}
    rec.ondataavailable=(ev)=>{if(ev.data&&ev.data.size)chunks.push(ev.data);};
    rec.onstop=async()=>{
      const type=chunks[0]?.type||'video/webm';
      const blob=new Blob(chunks,{type});
      const ts=new Date().toISOString().replace(/[:.]/g,'-');
      await saveBlobSmart(blob,`NozzleCAM_${ts}.webm`,type);
      setRecUI(false);
    };
    rec.start(1000); setRecUI(true);
  };

  window.addEventListener('orientationchange',()=>{img.style.transform='translateZ(0)';setTimeout(()=>img.style.transform='',100);});
  syncFSButton();
</script>

<div id="msg" style="position:fixed;bottom:1rem;left:50%;transform:translateX(-50%);background:#111;color:#fff;padding:.5rem 1rem;border-radius:.5rem;font-size:14px;display:none;z-index:999"></div>
<script>
function showMsg(text){const m=document.getElementById('msg');m.textContent=text;m.style.display='block';setTimeout(()=>m.style.display='none',3000);}
</script>
</body></html>
)HTML";
