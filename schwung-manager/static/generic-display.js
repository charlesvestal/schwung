const canvas = document.getElementById('c');
const ctx = canvas.getContext('2d');
const statusEl = document.getElementById('status');

let ws = null;
let imgData = null;
let currentWidth = 0;
let currentHeight = 0;
let frames = 0;
let lastFpsTime = Date.now();

function resizeFS() {
  if (!document.body.classList.contains('fs')) {
    canvas.style.width = '';
    canvas.style.height = '';
    return;
  }
  const w = window.innerWidth, h = window.innerHeight;
  const aspect = currentWidth / currentHeight;
  if (w / h > aspect) {
    canvas.style.height = h + 'px';
    canvas.style.width = (h * aspect) + 'px';
  } else {
    canvas.style.width = w + 'px';
    canvas.style.height = (w / aspect) + 'px';
  }
}

canvas.addEventListener('click', function() {
  document.body.classList.toggle('fs');
  resizeFS();
});
window.addEventListener('resize', resizeFS);

/* --- Touch / Mouse → WebSocket --- */

function canvasCoords(e) {
  const rect = canvas.getBoundingClientRect();
  const scaleX = currentWidth / rect.width;
  const scaleY = currentHeight / rect.height;
  let clientX, clientY;
  if (e.touches) {
    clientX = e.touches[0].clientX;
    clientY = e.touches[0].clientY;
  } else {
    clientX = e.clientX;
    clientY = e.clientY;
  }
  return {
    x: Math.round((clientX - rect.left) * scaleX),
    y: Math.round((clientY - rect.top) * scaleY)
  };
}

function sendTouch(e, state) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  if (state === 2 && !e.touches && !(e.buttons & 1)) return;
  const c = canvasCoords(e);
  ws.send(JSON.stringify({ x: c.x, y: c.y, state: state }));
  e.preventDefault();
}

canvas.addEventListener('mousedown',  e => sendTouch(e, 1));
canvas.addEventListener('mousemove',  e => sendTouch(e, 2));
canvas.addEventListener('mouseup',    e => sendTouch(e, 3));
canvas.addEventListener('touchstart', e => sendTouch(e, 1));
canvas.addEventListener('touchmove',  e => sendTouch(e, 2));
canvas.addEventListener('touchend',   e => sendTouch(e, 3));

/* --- WebSocket Frame Receive --- */

function handleFrame(arrayBuf) {
  const view = new DataView(arrayBuf);
  const w = view.getUint32(0, true);
  const h = view.getUint32(4, true);
  const bpp = view.getUint32(8, true);

  if (w !== currentWidth || h !== currentHeight) {
    currentWidth = w;
    currentHeight = h;
    canvas.width = w;
    canvas.height = h;
    imgData = ctx.createImageData(w, h);
    resizeFS();
  }

  const pixels = new Uint8Array(arrayBuf, 16);
  const d = imgData.data;

  if (bpp === 3) {
    for (let i = 0, j = 0; i < w * h; i++, j += 3) {
      const di = i * 4;
      d[di]     = pixels[j];
      d[di + 1] = pixels[j + 1];
      d[di + 2] = pixels[j + 2];
      d[di + 3] = 255;
    }
  } else if (bpp === 4) {
    d.set(pixels.subarray(0, w * h * 4));
  } else if (bpp === 1) {
    for (let i = 0; i < w * h; i++) {
      const di = i * 4;
      d[di] = d[di + 1] = d[di + 2] = pixels[i];
      d[di + 3] = 255;
    }
  }

  ctx.putImageData(imgData, 0, 0);

  frames++;
  const now = Date.now();
  if (now - lastFpsTime > 1000) {
    const fmt = bpp === 3 ? 'RGB' : bpp === 4 ? 'RGBA' : 'Gray';
    statusEl.textContent = 'connected - ' + w + 'x' + h + ' ' + fmt + ' - ' + frames + ' fps';
    frames = 0;
    lastFpsTime = now;
  }
}

/* --- Connect --- */

function connect() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(proto + '://' + location.host + '/ws-generic');
  ws.binaryType = 'arraybuffer';

  ws.onopen = function() {
    statusEl.textContent = 'connected';
    statusEl.className = 'connected';
  };

  ws.onmessage = function(e) {
    if (e.data instanceof ArrayBuffer) {
      handleFrame(e.data);
    }
  };

  ws.onclose = function() {
    statusEl.textContent = 'disconnected - reconnecting...';
    statusEl.className = '';
    setTimeout(connect, 2000);
  };

  ws.onerror = function() {
    ws.close();
  };
}

connect();
