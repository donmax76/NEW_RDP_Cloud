/**
 * Cloudflare Worker — прозрачный WebSocket прокси
 *
 * В netstat на хосте будет: TCP → 104.21.x.x:443 (Cloudflare IP)
 * Неотличимо от обычного браузерного HTTPS трафика.
 *
 * Деплой: https://workers.cloudflare.com → Create Worker → вставить этот код
 * Или через wrangler CLI: wrangler deploy
 */

// ── Конфигурация ───────────────────────────────────────────────
const BACKEND_HOST = 'YOUR_VPS_IP';   // IP или домен VPS (без протокола)
const BACKEND_PORT = 443;              // порт server.py на VPS
const BACKEND_TLS  = true;             // VPS использует TLS (wss://)

// Секрет: Worker добавляет этот заголовок к каждому запросу на VPS.
// server.py проверяет его — прямой доступ к VPS (минуя Worker) будет отклонён.
// Установи любую случайную строку, одинаковую здесь и в server.py
const WORKER_SECRET = 'ЗАМЕНИ_НА_СЛУЧАЙНУЮ_СТРОКУ_32_СИМВОЛА';
// ───────────────────────────────────────────────────────────────

export default {
  async fetch(request, env, ctx) {
    const host   = env.BACKEND_HOST   || BACKEND_HOST;
    const port   = env.BACKEND_PORT   || BACKEND_PORT;
    const tls    = (env.BACKEND_TLS   ?? BACKEND_TLS) !== false;
    const secret = env.WORKER_SECRET  || WORKER_SECRET;

    const upgradeHeader = request.headers.get('Upgrade');

    // Не WebSocket — вернуть пустую страницу (не давать 502 / fingerprint)
    if (!upgradeHeader || upgradeHeader.toLowerCase() !== 'websocket') {
      return new Response('OK', { status: 200 });
    }

    // Построить URL бэкенда (сохраняем path + query — они нужны для auth токена)
    const url = new URL(request.url);
    const scheme     = tls ? 'wss' : 'ws';
    const backendUrl = `${scheme}://${host}:${port}${url.pathname}${url.search}`;

    // Пробросить заголовки клиента (токен авторизации идёт в URL/заголовках)
    const fwdHeaders = new Headers();
    for (const [k, v] of request.headers) {
      if (k.toLowerCase() === 'host') continue; // не перебивать Host
      fwdHeaders.set(k, v);
    }
    // Подпись Worker → VPS, чтобы VPS принимал только через Worker
    if (secret) fwdHeaders.set('X-Relay-Secret', secret);

    // Подключиться к бэкенду
    let backendResp;
    try {
      backendResp = await fetch(backendUrl, {
        headers: fwdHeaders,
        // cf: { resolveOverride: host } // раскомментировать если нужен SNI override
      });
    } catch (e) {
      return new Response('Backend unreachable: ' + e.message, { status: 502 });
    }

    const backendWs = backendResp.webSocket;
    if (!backendWs) {
      return new Response('Backend did not upgrade to WebSocket', { status: 502 });
    }

    // Создать WebSocket пару для клиента (browser / host DLL)
    const { 0: client, 1: server } = new WebSocketPair();

    backendWs.accept();
    server.accept();

    // ── Клиент → Бэкенд ──
    server.addEventListener('message', ({ data }) => {
      try { backendWs.send(data); } catch (_) {}
    });
    server.addEventListener('close', ({ code, reason }) => {
      try { backendWs.close(code || 1000, reason || ''); } catch (_) {}
    });
    server.addEventListener('error', () => {
      try { backendWs.close(1011, 'client error'); } catch (_) {}
    });

    // ── Бэкенд → Клиент ──
    backendWs.addEventListener('message', ({ data }) => {
      try { server.send(data); } catch (_) {}
    });
    backendWs.addEventListener('close', ({ code, reason }) => {
      try { server.close(code || 1000, reason || ''); } catch (_) {}
    });
    backendWs.addEventListener('error', () => {
      try { server.close(1011, 'backend error'); } catch (_) {}
    });

    return new Response(null, { status: 101, webSocket: client });
  }
};
