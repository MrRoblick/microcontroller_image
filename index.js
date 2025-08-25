// index.js
const express = require('express');
const multer = require('multer');
const sharp = require('sharp');
const fetchFn = globalThis.fetch || (() => { try { return require('node-fetch'); } catch (e) { return null; } })();

const storage = multer.memoryStorage();
const upload = multer({ storage });

const app = express();
app.use(express.json({ limit: '20mb' }));

const FIXED_WIDTH = 240;
const FIXED_HEIGHT = 135;
const PIXEL_COUNT = FIXED_WIDTH * FIXED_HEIGHT;
const BYTE_COUNT = PIXEL_COUNT * 2;

// Default forward target; можно переопределить через переменную окружения TARGET_URL
const DEFAULT_TARGET = process.env.TARGET_URL || 'http://127.0.0.1:8080/update-image';

async function convertToRGB565Buffer(inputBuffer, opts = {}) {
  const { littleEndian = true, background = { r: 0, g: 0, b: 0 } } = opts;

  const { data, info } = await sharp(inputBuffer)
    .flatten({ background })
    .resize(FIXED_WIDTH, FIXED_HEIGHT, { fit: 'fill' })
    .raw()
    .toBuffer({ resolveWithObject: true });

  const channels = info.channels;
  if (channels < 3) throw new Error('Ожидаются по крайней мере 3 канала (RGB).');

  const out = Buffer.alloc(BYTE_COUNT);
  let src = 0;
  let dst = 0;
  for (let i = 0; i < PIXEL_COUNT; i++) {
    const r = data[src++], g = data[src++], b = data[src++];
    if (channels > 3) src++; // skip alpha

    const r5 = Math.round((r * 31) / 255) & 0x1f;
    const g6 = Math.round((g * 63) / 255) & 0x3f;
    const b5 = Math.round((b * 31) / 255) & 0x1f;
    const rgb565 = (r5 << 11) | (g6 << 5) | b5;

    const low = rgb565 & 0xff;
    const high = (rgb565 >> 8) & 0xff;

    if (littleEndian) {
      out[dst++] = low;
      out[dst++] = high;
    } else {
      out[dst++] = high;
      out[dst++] = low;
    }
  }
  return out;
}

/**
 * POST /convert
 * - multipart/form-data field "image" OR form field "imageUrl"
 * - optional headers:
 *    X-Forward-To (override default forward target)
 *    X-Endian (little|big) -- default little
 *    X-BG (R,G,B) optional background color for flattening
 *
 * Returns JSON/text with forwarding result.
 */
app.post('/convert', upload.single('image'), async (req, res) => {
  try {
    const headerTarget = req.header('X-Forward-To');
    const targetUrl = headerTarget || DEFAULT_TARGET;

    const headerEndian = (req.header('X-Endian') || 'little').toLowerCase();
    const littleEndian = headerEndian !== 'big';

    const headerBg = req.header('X-BG') || req.body.bg || '0,0,0';
    const bgParts = String(headerBg).split(',').map(n => Number(n.trim()) || 0);
    const background = { r: bgParts[0] ?? 0, g: bgParts[1] ?? 0, b: bgParts[2] ?? 0 };

    // get input buffer
    let inputBuffer = null;
    if (req.file && req.file.buffer) {
      inputBuffer = req.file.buffer;
    } else if (req.body && req.body.imageUrl) {
      if (!fetchFn) return res.status(500).send('fetch not available on this Node runtime; install node-fetch or send file.');
      const r = await fetchFn(req.body.imageUrl);
      if (!r.ok) return res.status(400).send('Failed to download image URL.');
      inputBuffer = Buffer.from(await r.arrayBuffer());
    } else if (req.body && req.body.imageUrl === undefined && !req.file) {
      // also support imageUrl sent as form field (multipart)
      if (req.body && req.body.imageUrl) {
        if (!fetchFn) return res.status(500).send('fetch not available on this Node runtime; install node-fetch or send file.');
        const r = await fetchFn(req.body.imageUrl);
        if (!r.ok) return res.status(400).send('Failed to download image URL.');
        inputBuffer = Buffer.from(await r.arrayBuffer());
      }
    }

    if (!inputBuffer) return res.status(400).send('No image provided. Send multipart file `image` or field `imageUrl`.');

    const outBuf = await convertToRGB565Buffer(inputBuffer, { littleEndian, background });

    // Forward to target via POST with binary body
    if (!fetchFn) return res.status(500).send('fetch not available to forward to target; install node-fetch or use Node 18+');

    const forwardResp = await fetchFn(targetUrl, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/octet-stream',
        'Content-Length': String(outBuf.length),
        'X-Width': String(FIXED_WIDTH),
        'X-Height': String(FIXED_HEIGHT),
        'X-Endian': littleEndian ? 'little' : 'big'
      },
      body: outBuf
    });

    const fwdText = await (forwardResp.text().catch(() => Promise.resolve('')));

    res.setHeader('Content-Type', 'application/json; charset=utf-8');
    return res.status(200).send(JSON.stringify({
      ok: forwardResp.ok,
      forwardedTo: targetUrl,
      forwardStatus: forwardResp.status,
      forwardText: fwdText
    }));
  } catch (err) {
    console.error(err);
    res.status(500).send('Server error: ' + err.message);
  }
});

// Serve the upload page (upload.html should exist in project root)
const path = require('path');
app.get('/', (req, res) => res.sendFile(path.join(__dirname, 'upload.html')));

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => console.log(`Server listening on http://localhost:${PORT}`));
