# Glane Notes — Inbox Webhook (Cloudflare Worker → Notion)

A small Cloudflare Worker that receives transcribed notes from the Glane Notes
device and files them into a Notion database. The device never talks to Notion
directly: this Worker holds the Notion credentials, dedups by note id, and
creates the page, so the firmware stays stable even if the backend or Notion
schema changes.

```text
ESP32 ──POST /inbox {id,created,duration_s,text}──▶ Worker ──▶ Notion API
```

## What it needs

Before deployment, prepare:

- A Cloudflare account with Workers enabled
- A Notion integration token
- A Notion database shared with that integration
- Three Worker secrets:
  - `INBOX_TOKEN`
  - `NOTION_TOKEN`
  - `NOTION_DB_ID`

## 1. Create the Notion side

Create a database named e.g. **Glane Inbox** with these properties. Property
names are case-sensitive because the Worker uses them directly.

- `Name` — **Title** (holds the note id)
- `Created` — **Date**
- `Duration` — **Number**
- `Source` — **Rich text**
- `Status` — **Select** with an option named `Inbox`

### Create the integration

1. Open <https://www.notion.so/my-integrations>.
2. Create a new internal connection/integration.
3. Copy its token.

Notes:

- In older Notion docs this may be called **Internal Integration Secret**.
- In the current UI it often appears as **Access token**.
- This token is the value for `NOTION_TOKEN`.

### Share the database with the integration

Open the target database and connect/share it with your integration:

- Database page → **...**
- **Connections**
- Select your integration

If the database is not connected, Notion API calls will fail with
`object_not_found` even if the token itself is valid.

### Find `NOTION_DB_ID`

Use the database ID, not the page view ID.

- Open the database itself
- Prefer **Open as full page** if it is currently inline inside another page
- Copy the URL
- Use the 32-character hex id in the path, not the `?v=...` value

Example:

```text
https://app.notion.com/p/thoughtfulness/386dd12f139980498583fa80232fe8b9?v=386dd12f13998087a6e7000c9c646af5
```

In this example:

- `386dd12f139980498583fa80232fe8b9` is `NOTION_DB_ID`
- `386dd12f13998087a6e7000c9c646af5` is a view id, not the database id

You can also verify the id directly with the Notion API:

```bash
curl https://api.notion.com/v1/databases/$NOTION_DB_ID \
  -H "Authorization: Bearer $NOTION_TOKEN" \
  -H "Notion-Version: 2022-06-28"
```

If this returns the database JSON, the token and database id are correct.

## 2. Configure Worker secrets

Choose any strong random string for `INBOX_TOKEN`. This is the shared bearer
token your device sends to the Worker; it is not provided by Cloudflare or
Notion.

```bash
cd server/inbox-worker
npm install

npx wrangler secret put INBOX_TOKEN
npx wrangler secret put NOTION_TOKEN
npx wrangler secret put NOTION_DB_ID
```

Secret meanings:

- `INBOX_TOKEN`: shared auth token sent by the device
- `NOTION_TOKEN`: Notion integration access token
- `NOTION_DB_ID`: target Notion database id

## 3. Deploy the Worker

```bash
cd server/inbox-worker
npx wrangler deploy
```

Notes:

- Recent Wrangler versions may not support `wrangler deploy --temporary`.
- If your local environment blocks Wrangler from writing debug logs, this can
  help:

```bash
WRANGLER_LOG=none npx wrangler deploy
```

After deploy, Cloudflare prints a `workers.dev` URL for the Worker.

## 4. Point the device at it

On the device dashboard, set:

- **Inbox webhook URL**: `https://<your-worker>.workers.dev/inbox`
- **Inbox token**: the same value you used for `INBOX_TOKEN`

Now every sync, after a note is transcribed, the device POSTs it here and it
lands in your Notion inbox. Re-syncs are deduped by note id, so repeated sends
do not create duplicate pages.

### Device config example

Example final values after deployment:

```text
Inbox webhook URL: https://glane-notes-inbox.<your-subdomain>.workers.dev/inbox
Inbox token:      <the same INBOX_TOKEN you stored in Worker secrets>
```

Checklist before enabling it on the device:

- The Worker root URL responds with `{"ok":true,"service":"glane-notes-inbox"}`
- A manual `POST /inbox` test succeeds
- The device uses the same `INBOX_TOKEN` as the Worker secret
- The Notion database is still connected to the integration

Recommended rollout:

1. Save the webhook URL and token on the device.
2. Trigger one sync with a short test note.
3. Confirm a new page appears in Notion.
4. Trigger the same sync again and verify it does not create a duplicate.

## 5. Test it manually

Health check:

```bash
curl https://<your-worker>.workers.dev/
```

Expected response:

```json
{"ok":true,"service":"glane-notes-inbox"}
```

Create a note:

```bash
curl -X POST https://<your-worker>.workers.dev/inbox \
  -H "Authorization: Bearer $INBOX_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"id":"note-test-1","created":"2026-06-21T13:00:00Z","duration_s":12,"source":"glane-notes","text":"hello from glane notes"}'
```

Expected success response:

```json
{"ok":true,"page":"<notion-page-id>"}
```

If you send the exact same payload again, the Worker should dedup it:

```json
{"ok":true,"deduped":true,"page":"<notion-page-id>"}
```

End-to-end sanity check after device setup:

```text
Device records note
-> Device syncs
-> Worker receives POST /inbox
-> Worker writes one page to Notion
-> Repeated sync of the same note returns deduped=true
```

## Troubleshooting

### `401 unauthorized`

- `INBOX_TOKEN` on the device does not match the Worker secret
- Request is missing `Authorization: Bearer <token>`

### `500 worker not configured (NOTION_TOKEN/NOTION_DB_ID)`

- `NOTION_TOKEN` or `NOTION_DB_ID` has not been set in Worker secrets

### `502 notion query failed` with `object_not_found`

- The database is not shared with the integration
- `NOTION_DB_ID` is wrong
- You copied a page id or view id instead of the database id
- The Worker is still using an old `NOTION_TOKEN`; re-run `wrangler secret put`
  and redeploy

### `502 notion create failed`

- Database property names do not match what the Worker expects
- Required properties such as `Name` or `Status` are missing
- The `Status` select does not include an `Inbox` option

### Token exposed accidentally

If a Notion token was pasted into chat, screenshots, logs, or shell history,
rotate it in Notion and update the Worker secret again:

```bash
npx wrangler secret put NOTION_TOKEN
npx wrangler deploy
```

## Request contract

| field        | type   | notes                                |
|--------------|--------|--------------------------------------|
| `id`         | string | required; unique note id (dedup key) |
| `text`       | string | transcript (UTF-8)                   |
| `created`    | string | ISO-8601, optional                   |
| `duration_s` | number | optional                             |
| `source`     | string | optional, defaults to `glane-notes`  |

Responses:

- `200 {ok:true,page}` on create
- `200 {ok:true,deduped:true}` if the note already exists
- `401` for bad token
- `400` for bad body
- `502` for Notion-side errors
