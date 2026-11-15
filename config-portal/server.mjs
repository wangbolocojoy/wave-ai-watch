import { createServer } from 'node:http';
import { mkdir, readFile, rename, writeFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { isIP } from 'node:net';

const root = dirname(fileURLToPath(import.meta.url));
const configPath = join(root, 'data', 'watch-config.json');
const labDir = join(root, 'public', 'lab');
const sessionPath = join(root, 'data', 'device-session.json');
const faces = ['luxe', 'sport', 'classic', 'cute'];
const legacyFaces = { aurora: 'luxe', minimal: 'luxe', orbit: 'sport', noir: 'classic' };
const languages = ['zh-CN'];
let deviceSession = null;
const defaults = {
  face: 'cute',
  brightness: 72,
  volume: 55,
  doNotDisturb: false,
  assistantEnabled: true,
  wakeWord: '小智小智',
  language: 'zh-CN',
  complications: { weather: true, steps: true, battery: true },
};

async function loadDeviceSession() {
  try {
    const saved = JSON.parse(await readFile(sessionPath, 'utf8'));
    if (saved && isPrivateDeviceAddress(saved.host) &&
        typeof saved.token === 'string' && /^[0-9a-f]{32}$/.test(saved.token)) {
      deviceSession = { host: saved.host, token: saved.token };
    }
  } catch (error) {
    if (error.code !== 'ENOENT') console.warn('device session restore failed:', error.message);
  }
}

async function saveDeviceSession() {
  await mkdir(dirname(sessionPath), { recursive: true });
  if (deviceSession) {
    await writeFile(sessionPath, JSON.stringify(deviceSession, null, 2));
  } else {
    await rename(sessionPath, `${sessionPath}.bak`).catch(() => {});
  }
}

async function loadConfig() {
  try {
    return validateConfig(JSON.parse(await readFile(configPath, 'utf8')));
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
    await saveConfig(defaults);
    return defaults;
  }
}

function validateConfig(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error('配置必须是 JSON 对象');
  }
  const intInRange = (n, min, max) => Number.isInteger(n) && n >= min && n <= max;
  const face = legacyFaces[value.face] || value.face;
  if (!faces.includes(face)) throw new Error('表盘选项无效');
  if (!intInRange(value.brightness, 10, 100)) throw new Error('亮度必须在 10–100 之间');
  if (!intInRange(value.volume, 0, 100)) throw new Error('音量必须在 0–100 之间');
  if (typeof value.doNotDisturb !== 'boolean' || typeof value.assistantEnabled !== 'boolean') {
    throw new Error('开关配置无效');
  }
  if (typeof value.wakeWord !== 'string' || value.wakeWord.trim().length < 2 || value.wakeWord.length > 24) {
    throw new Error('唤醒词长度需为 2–24 个字符');
  }
  if (!languages.includes(value.language)) throw new Error('语言选项无效');
  const c = value.complications;
  if (!c || ['weather', 'steps', 'battery'].some(key => typeof c[key] !== 'boolean')) {
    throw new Error('表盘组件配置无效');
  }
  return {
    ...defaults,
    face,
    brightness: value.brightness,
    volume: value.volume,
    doNotDisturb: value.doNotDisturb,
    assistantEnabled: value.assistantEnabled,
    wakeWord: value.wakeWord.trim(),
    language: value.language,
    complications: { weather: c.weather, steps: c.steps, battery: c.battery },
  };
}

async function saveConfig(config) {
  await mkdir(dirname(configPath), { recursive: true });
  const tempPath = `${configPath}.tmp`;
  await writeFile(tempPath, `${JSON.stringify(config, null, 2)}\n`, 'utf8');
  await rename(tempPath, configPath);
}

function sendJson(response, status, payload) {
  response.writeHead(status, { 'content-type': 'application/json; charset=utf-8' });
  response.end(JSON.stringify(payload));
}

async function readRequestJson(request) {
  let body = '';
  for await (const chunk of request) {
    body += chunk;
    if (body.length > 16_384) throw new Error('配置请求太大');
  }
  return JSON.parse(body);
}

function isPrivateDeviceAddress(host) {
  if (typeof host !== 'string' || isIP(host) !== 4) return false;
  const octets = host.split('.').map(Number);
  if (octets[3] === 0 || octets[3] === 255) return false;
  return octets[0] === 10 ||
    (octets[0] === 172 && octets[1] >= 16 && octets[1] <= 31) ||
    (octets[0] === 192 && octets[1] === 168);
}

async function requestDevice(path, method = 'GET', body) {
  if (!deviceSession) throw new Error('请先连接手表');
  const response = await fetch(`http://${deviceSession.host}${path}`, {
    method,
    headers: {
      authorization: `Bearer ${deviceSession.token}`,
      ...(body === undefined ? {} : { 'content-type': 'application/json' }),
    },
    body: body === undefined ? undefined : JSON.stringify(body),
    cache: 'no-store',
    redirect: 'error',
    signal: AbortSignal.timeout(4000),
  });
  const result = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(result.error || `手表返回 HTTP ${response.status}`);
  return result;
}

function rejectRemoteOrigin(request, response) {
  const origin = request.headers.origin;
  if (!origin) return false;
  const allowed = new Set([
    `http://127.0.0.1:${process.env.WATCH_CONFIG_PORT || 8765}`,
    `http://localhost:${process.env.WATCH_CONFIG_PORT || 8765}`,
  ]);
  if (allowed.has(origin)) return false;
  sendJson(response, 403, { error: '只允许从本机配置页面发起设备操作' });
  return true;
}

const server = createServer(async (request, response) => {
  try {
    if ((request.method === 'POST' || request.method === 'PUT') &&
        request.url.startsWith('/api/device/') && rejectRemoteOrigin(request, response)) {
      return;
    }
    if (request.method === 'GET' && request.url === '/api/config') {
      sendJson(response, 200, await loadConfig());
      return;
    }
    if (request.method === 'PUT' && request.url === '/api/config') {
      const config = validateConfig(await readRequestJson(request));
      await saveConfig(config);
      sendJson(response, 200, { ok: true, config });
      return;
    }
    if (request.method === 'GET' && request.url === '/api/device/status') {
      sendJson(response, 200, { connected: deviceSession !== null,
        host: deviceSession?.host ?? null });
      return;
    }
    if (request.method === 'POST' && request.url === '/api/device/pair') {
      const { host, code = '' } = await readRequestJson(request);
      if (!isPrivateDeviceAddress(host) ||
          (typeof code === 'string' && code.length > 0 && !/^\d{6,8}$/.test(code))) {
        sendJson(response, 400, { error: '请输入手表的局域网 IPv4 地址；配对码可留空' });
        return;
      }
      const pairResponse = await fetch(`http://${host}/api/watch/pair`, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ code: typeof code === 'string' ? code : '' }),
        cache: 'no-store',
        redirect: 'error',
        signal: AbortSignal.timeout(4000),
      });
      const pairResult = await pairResponse.json().catch(() => ({}));
      if (!pairResponse.ok || typeof pairResult.token !== 'string' || !/^[0-9a-f]{32}$/.test(pairResult.token)) {
        sendJson(response, pairResponse.ok ? 502 : pairResponse.status,
          { error: pairResult.error || '配对失败，请检查配对码并重试' });
        return;
      }
      deviceSession = { host, token: pairResult.token };
      await saveDeviceSession();
      try {
        const config = await requestDevice('/api/watch/config');
        sendJson(response, 200, { ok: true, host, config });
      } catch (error) {
        deviceSession = null;
        sendJson(response, 502, { error: error.message || '已配对但读取设置失败' });
      }
      return;
    }
    if (request.method === 'GET' && request.url === '/api/device/screen') {
      if (!deviceSession) {
        sendJson(response, 409, { error: '请先连接手表' });
        return;
      }
      const screenResponse = await fetch(`http://${deviceSession.host}/api/watch/screen`, {
        headers: { authorization: `Bearer ${deviceSession.token}` },
        cache: 'no-store',
        redirect: 'error',
        signal: AbortSignal.timeout(30000),
      });
      if (!screenResponse.ok) {
        const detail = await screenResponse.text().catch(() => '');
        sendJson(response, screenResponse.status, { error: detail || '手表截图失败' });
        return;
      }
      const image = Buffer.from(await screenResponse.arrayBuffer());
      response.writeHead(200, {
        'content-type': screenResponse.headers.get('content-type') || 'image/jpeg',
        'cache-control': 'no-store',
        'content-length': image.length,
      });
      response.end(image);
      return;
    }
    if (request.method === 'POST' && request.url === '/api/device/navigate') {
      const { page } = await readRequestJson(request);
      const allowed = new Set(['设置', '应用', '音乐', '活动', '倒计时', '闹钟', '小智', '大肥鱼', '语音备忘录', '录音', '表盘', '主页', '首页', 'AOD', '唤醒', '通知', '测试通知', '播放音乐']);
      if (!allowed.has(page)) {
        sendJson(response, 400, { error: '不支持的页面' });
        return;
      }
      const result = await requestDevice('/api/watch/navigate', 'POST', { page });
      sendJson(response, 200, result);
      return;
    }
    if (request.method === 'GET' && request.url === '/api/device/config') {
      sendJson(response, 200, await requestDevice('/api/watch/config'));
      return;
    }
    if (request.method === 'PUT' && request.url === '/api/device/config') {
      const config = validateConfig(await readRequestJson(request));
      const applied = await requestDevice('/api/watch/config', 'PUT', {
        face: config.face,
        brightness: config.brightness,
        volume: config.volume,
        doNotDisturb: config.doNotDisturb,
      });
      sendJson(response, 200, { ok: true, applied, config });
      return;
    }
    if (request.method === 'POST' && request.url === '/api/device/disconnect') {
      if (deviceSession) {
        try { await requestDevice('/api/watch/disconnect', 'POST'); } catch {}
      }
      deviceSession = null;
      await saveDeviceSession();
      sendJson(response, 200, { ok: true });
      return;
    }
    if (request.method === 'GET' && (request.url === '/' || request.url === '/simulator' || request.url === '/simulator.html')) {
      const html = await readFile(join(root, 'public', 'simulator.html'));
      response.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
      response.end(html);
      return;
    }
    if (request.method === 'GET' && ['/settings', '/settings.html', '/index.html'].includes(request.url)) {
      const html = await readFile(join(root, 'public', 'index.html'));
      response.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
      response.end(html);
      return;
    }
    if (request.method === 'GET' && /^\/assets\/(face-(luxe|sport|classic|cute)-zh|watch-device)\.png$/.test(request.url)) {
      const filename = request.url.slice('/assets/'.length);
      const image = await readFile(join(root, 'public', 'assets', filename));
      response.writeHead(200, { 'content-type': 'image/png', 'cache-control': 'public, max-age=3600' });
      response.end(image);
      return;
    }
    if (request.method === 'GET' && request.url === '/lab') {
      response.writeHead(302, { location: '/lab/' });
      response.end();
      return;
    }
    if (request.method === 'GET' && request.url.startsWith('/lab/')) {
      const filename = request.url === '/lab/' ? 'index.html' : request.url.slice('/lab/'.length);
      if (!/^[A-Za-z0-9._-]+\.(html|css|js|svg|png|woff2)$/.test(filename)) {
        sendJson(response, 404, { error: 'Not found' });
        return;
      }
      const mime = {
        '.html': 'text/html; charset=utf-8',
        '.css': 'text/css; charset=utf-8',
        '.js': 'text/javascript; charset=utf-8',
        '.svg': 'image/svg+xml',
        '.png': 'image/png',
        '.woff2': 'font/woff2',
      }[filename.slice(filename.lastIndexOf('.'))] || 'application/octet-stream';
      const body = await readFile(join(labDir, filename));
      response.writeHead(200, { 'content-type': mime, 'cache-control': 'no-store' });
      response.end(body);
      return;
    }
    sendJson(response, 404, { error: 'Not found' });
  } catch (error) {
    sendJson(response, 400, { error: error.message });
  }
});

const host = '127.0.0.1';
const port = Number(process.env.WATCH_CONFIG_PORT || 8765);
await loadDeviceSession();
server.listen(port, host, () => {
  console.log(`Watch config portal: http://${host}:${port}`);
});
