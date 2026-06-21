# Glane Notes — Inbox Webhook (Cloudflare Worker → Notion)

A thin Cloudflare Worker that receives transcribed notes from the Glane Notes
device and files them into a Notion **Inbox** database (PARA-style capture). The
device never talks to Notion directly — this Worker holds the Notion token,
dedups by note id, and builds the page, so the firmware stays stable even if the
Notion schema or backend changes.

```
ESP32 ──POST /inbox {id,created,duration_s,text}──▶ Worker ──▶ Notion API
```

## 1. Create the Notion side

1. Create a database (full page) named e.g. **Glane Inbox** with these
   properties (names are case-sensitive):
   - `Name` — **Title**  (holds the note id)
   - `Created` — **Date**
   - `Duration` — **Number**
   - `Source` — **Text**
   - `Status` — **Select** (add an `Inbox` option)
2. Create an internal integration at <https://www.notion.so/my-integrations>,
   copy its **Internal Integration Secret** (`secret_…`).
3. Open the database → **⋯ → Connections → Connect to** your integration.
4. Copy the **database id** from the database URL: the 32-hex chunk before `?v=`.

## 2. Deploy the Worker

```bash
cd server/inbox-worker
npm install

# Quick throwaway test (no Cloudflare account needed up front):
npx wrangler deploy --temporary      # prints a *.workers.dev URL + a claim URL

# Or a permanent deploy (after `wrangler login`):
npx wrangler deploy
```

Set the three secrets (pick any strong value for `INBOX_TOKEN`):

```bash
npx wrangler secret put INBOX_TOKEN     # shared token the device sends
npx wrangler secret put NOTION_TOKEN    # secret_… from step 1.2
npx wrangler secret put NOTION_DB_ID    # database id from step 1.4
```

> Using `--temporary`? The account self-destructs after 60 min unless you click
> the printed **claim URL** to keep it.

## 3. Point the device at it

On the device dashboard → **Settings**:
- **Inbox webhook URL**: `https://<your-worker>.workers.dev/inbox`
- **Inbox token**: the same value you set for `INBOX_TOKEN`

Now every sync, after a note is transcribed, the device POSTs it here and it
lands in your Notion Inbox. Re-syncs are deduped by note id (the device also
writes a local `.pushed` marker), so no duplicate pages.

## Test it by hand

```bash
curl -X POST https://<your-worker>.workers.dev/inbox \
  -H "Authorization: Bearer $INBOX_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"id":"note-test-1","created":"2026-06-21T13:00:00Z","duration_s":12,"source":"glane-notes","text":"hello from glane notes"}'
```

A second identical call returns `{"ok":true,"deduped":true,...}` without
creating another page.

## Request contract

| field        | type   | notes                                  |
|--------------|--------|----------------------------------------|
| `id`         | string | required; unique note id (dedup key)   |
| `text`       | string | transcript (UTF-8)                     |
| `created`    | string | ISO-8601, optional                     |
| `duration_s` | number | optional                               |
| `source`     | string | optional, defaults to `glane-notes`    |

Responses: `200 {ok:true,page}` on create, `200 {ok:true,deduped:true}` if it
already exists, `401` bad token, `400` bad body, `502` Notion error.
