/**
 * Cloudflare Worker + D1 — Sensor de sala
 * Binding do D1 no Worker: DB
 */

const JANELA_DESPERDICIO_MS = 120000; // 2 minutos

function json(dados, status = 200) {
  return new Response(JSON.stringify(dados, null, 2), {
    status,
    headers: { "Content-Type": "application/json; charset=utf-8" },
  });
}

// ---------- POST ----------

async function postMovimento(env) {
  const ts = Date.now();
  const r = await env.DB
    .prepare("INSERT INTO movimentos (ts) VALUES (?1)")
    .bind(ts)
    .run();
  return json({ ok: true, id: r.meta.last_row_id, ts }, 201);
}

async function postLuz(request, env) {
  let corpo;
  try {
    corpo = await request.json();
  } catch {
    return json({ erro: "JSON inválido" }, 400);
  }

  const valor = Number(corpo.valor);
  const acesa = Number(corpo.acesa);

  if (!Number.isInteger(valor) || valor < 0 || valor > 4095) {
    return json({ erro: "'valor' deve ser inteiro entre 0 e 4095" }, 400);
  }
  if (acesa !== 0 && acesa !== 1) {
    return json({ erro: "'acesa' deve ser 0 ou 1" }, 400);
  }

  const ts = Date.now();
  const r = await env.DB
    .prepare("INSERT INTO luz (valor, acesa, ts) VALUES (?1, ?2, ?3)")
    .bind(valor, acesa, ts)
    .run();
  return json({ ok: true, id: r.meta.last_row_id, valor, acesa, ts }, 201);
}

// ---------- GET ----------

async function getUltimoMovimento(env) {
  const linha = await env.DB
    .prepare("SELECT MAX(ts) AS ts FROM movimentos")
    .first();

  if (!linha || linha.ts === null) {
    return json({ erro: "Nenhum movimento registrado ainda" }, 404);
  }
  const segundos = Math.floor((Date.now() - linha.ts) / 1000);
  return json({ segundos, ultimo_ts: linha.ts });
}

async function getPorHora(env) {
  const { results } = await env.DB
    .prepare(
      `SELECT strftime('%H', ts / 1000, 'unixepoch', '-3 hours') AS hora,
              COUNT(*) AS movimentos
         FROM movimentos
        GROUP BY hora
        ORDER BY hora`
    )
    .all();
  return json({ fuso: "America/Sao_Paulo (UTC-3)", horas: results });
}

async function getLuzResumo(env) {
  const linha = await env.DB
    .prepare(
      `SELECT COUNT(*) AS total,
              COALESCE(SUM(acesa), 0) AS acesas,
              ROUND(100.0 * COALESCE(SUM(acesa), 0) / NULLIF(COUNT(*), 0), 1) AS porcentagem
         FROM luz`
    )
    .first();
  return json({
    total: linha.total,
    acesas: linha.acesas,
    porcentagem: linha.porcentagem ?? 0,
  });
}

async function getDesperdicio(env) {
  const linha = await env.DB
    .prepare(
      `SELECT COUNT(*) AS desperdicio
         FROM luz
        WHERE acesa = 1
          AND NOT EXISTS (
                SELECT 1 FROM movimentos m
                 WHERE m.ts BETWEEN luz.ts - ?1 AND luz.ts
              )`
    )
    .bind(JANELA_DESPERDICIO_MS)
    .first();
  return json({ desperdicio: linha.desperdicio, janela_ms: JANELA_DESPERDICIO_MS });
}

// ---------- Roteador ----------

const ROTAS = {
  "/movimento":        { POST: (req, env) => postMovimento(env) },
  "/luz":              { POST: (req, env) => postLuz(req, env) },
  "/ultimo-movimento": { GET:  (req, env) => getUltimoMovimento(env) },
  "/por-hora":         { GET:  (req, env) => getPorHora(env) },
  "/luz-resumo":       { GET:  (req, env) => getLuzResumo(env) },
  "/desperdicio":      { GET:  (req, env) => getDesperdicio(env) },
};

export default {
  async fetch(request, env) {
    const { pathname } = new URL(request.url);
    const rota = ROTAS[pathname];

    if (!rota) return json({ erro: "Rota não encontrada" }, 404);

    const handler = rota[request.method];
    if (!handler) {
      return new Response(JSON.stringify({ erro: "Método não permitido" }), {
        status: 405,
        headers: {
          "Content-Type": "application/json; charset=utf-8",
          Allow: Object.keys(rota).join(", "),
        },
      });
    }

    try {
      return await handler(request, env);
    } catch (e) {
      console.error(e); // detalhe fica só nos logs do Worker, não vai para o cliente
      return json({ erro: "Erro interno" }, 500);
    }
  },
};
