// Glane Notes — inbox webhook (Cloudflare Worker → Notion)
//
// The device POSTs a small JSON payload here after transcribing a note; this
// Worker holds the Notion credentials, dedups by note id, and creates a page in
// a Notion "Inbox" database. Keeping Notion's schema out of the firmware means
// you can change the database, page layout, or even the backend without
// reflashing the device.
//
// Expected request:
//   POST /inbox
//   Authorization: Bearer <INBOX_TOKEN>
//   Content-Type: application/json
//   { "id":"note-0000000042", "created":"2026-06-21T13:00:00Z",
//     "duration_s":37, "source":"glane-notes", "text":"…transcript…" }
//
// Secrets / vars (set via `wrangler secret put` or the dashboard):
//   INBOX_TOKEN   shared bearer token the device must present
//   NOTION_TOKEN  Notion internal integration token (secret_…)
//   NOTION_DB_ID  target Inbox database id
//
// Notion Inbox database properties used (create these, names are case-sensitive):
//   Name      (title)   ← note id
//   Created   (date)
//   Duration  (number)
//   Source    (rich_text)
//   Status    (select)  ← set to "Inbox"

const NOTION_VERSION = "2022-06-28";

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (request.method === "GET" && url.pathname === "/") {
      return json({ ok: true, service: "glane-notes-inbox" });
    }
    if (request.method !== "POST" || url.pathname !== "/inbox") {
      return json({ error: "not found" }, 404);
    }

    // ── Auth ──
    if (env.INBOX_TOKEN) {
      const auth = request.headers.get("authorization") || "";
      const tok = auth.startsWith("Bearer ") ? auth.slice(7) : "";
      if (tok !== env.INBOX_TOKEN) return json({ error: "unauthorized" }, 401);
    }

    // ── Parse ──
    let note;
    try {
      note = await request.json();
    } catch {
      return json({ error: "invalid json" }, 400);
    }
    const id = (note.id || "").toString().trim();
    if (!id) return json({ error: "missing id" }, 400);
    const text = (note.text || "").toString();
    const created = (note.created || "").toString();
    const source = (note.source || "glane-notes").toString();
    const duration = Number.isFinite(note.duration_s) ? note.duration_s : null;

    if (!env.NOTION_TOKEN || !env.NOTION_DB_ID) {
      return json({ error: "worker not configured (NOTION_TOKEN/NOTION_DB_ID)" }, 500);
    }

    // ── Dedup: skip if a page with this id already exists ──
    try {
      const existing = await notionFindById(env, id);
      if (existing) return json({ ok: true, deduped: true, page: existing });
    } catch (e) {
      return json({ error: "notion query failed", detail: String(e) }, 502);
    }

    // ── Create the Notion page ──
    try {
      const page = await notionCreatePage(env, { id, text, created, source, duration });
      return json({ ok: true, page: page.id });
    } catch (e) {
      return json({ error: "notion create failed", detail: String(e) }, 502);
    }
  },
};

function json(obj, status = 200) {
  return new Response(JSON.stringify(obj), {
    status,
    headers: { "content-type": "application/json" },
  });
}

async function notionFindById(env, id) {
  const res = await fetch(`https://api.notion.com/v1/databases/${env.NOTION_DB_ID}/query`, {
    method: "POST",
    headers: notionHeaders(env),
    body: JSON.stringify({
      filter: { property: "Name", title: { equals: id } },
      page_size: 1,
    }),
  });
  if (!res.ok) throw new Error(`query ${res.status}: ${await res.text()}`);
  const data = await res.json();
  return data.results && data.results.length ? data.results[0].id : null;
}

async function notionCreatePage(env, { id, text, created, source, duration }) {
  const properties = {
    Name: { title: [{ text: { content: id } }] },
    Source: { rich_text: [{ text: { content: source } }] },
    Status: { select: { name: "Inbox" } },
  };
  if (created) properties.Created = { date: { start: created } };
  if (duration != null) properties.Duration = { number: duration };

  const body = {
    parent: { database_id: env.NOTION_DB_ID },
    properties,
    children: textToBlocks(text),
  };

  const res = await fetch("https://api.notion.com/v1/pages", {
    method: "POST",
    headers: notionHeaders(env),
    body: JSON.stringify(body),
  });
  if (!res.ok) throw new Error(`create ${res.status}: ${await res.text()}`);
  return res.json();
}

function notionHeaders(env) {
  return {
    Authorization: `Bearer ${env.NOTION_TOKEN}`,
    "Notion-Version": NOTION_VERSION,
    "Content-Type": "application/json",
  };
}

// Notion limits a rich_text content chunk to 2000 chars and paragraphs per
// request; split the transcript into paragraph blocks of <=1900 chars.
function textToBlocks(text) {
  const blocks = [];
  const trimmed = (text || "").trim();
  if (!trimmed) return blocks;
  for (const para of trimmed.split(/\n{2,}/)) {
    let s = para.trim();
    if (!s) continue;
    while (s.length > 1900) {
      blocks.push(paragraph(s.slice(0, 1900)));
      s = s.slice(1900);
    }
    blocks.push(paragraph(s));
    if (blocks.length >= 95) break; // Notion caps children at 100 per request
  }
  return blocks;
}

function paragraph(content) {
  return {
    object: "block",
    type: "paragraph",
    paragraph: { rich_text: [{ type: "text", text: { content } }] },
  };
}
