# Self-hosting the web port of Prince of Persia 2 (Macintosh)

This is a **static** web app. There is no server code, nothing to build in order to
*run* it, and — unlike most WebAssembly games — it needs **no `COOP`/`COEP` /
cross-origin-isolation headers**. It runs on effectively any static host.

The engine ships **without game data**. You point it at a prepared data archive
that you make once from your own retail copy of the Macintosh *Prince of Persia 2*.

---

## What's in the release

| File | What it is |
| --- | --- |
| `index.html`, `pop2.js`, `pop2.wasm` | the engine (the browser recompilation) — **no game data** |
| `embed.html` | a lightweight "click-to-load" poster for embedding in a page |
| `embed-code.html` | a copy-paste `<iframe>` snippet generator |
| `og-image.png`, `favicon.*` | social/tab icons (optional) |

~12 MB total. The game data (~15 MB, see below) is **not** included.

---

## 1 · Put the files on any static host

Upload the folder to GitHub Pages, Netlify, Vercel, Cloudflare Pages, an S3 bucket,
itch.io, or plain nginx/Apache. Two things to check:

- **`.wasm` is served as `application/wasm`.** Almost every host does this
  automatically; a few old ones need a one-line MIME mapping.
- **No special headers are needed.** (If your host injects a strict
  `Content-Security-Policy`, allow the page's own inline script and WebAssembly.)

That's it — the engine now loads. It will show a drop-zone until it has data.

---

## 2 · Prepare the game data (one-time, on your computer)

The browser can only load a **prepared** `pop2-data.tar.gz`. It cannot unpack a raw
retail StuffIt (`.sit`) / RAR in the browser — that extraction is a desktop step
(it's what this project's own CI does at build time). Do it once, from your own copy,
using the tools in the source repo:

```sh
git clone https://github.com/mac-recomp/pop2 && cd pop2
# needs: unar (The Unarchiver) + python3
tools/prep-web-data.sh  "Prince_Of_Persia2.sit"  /tmp/pop2      # -> /tmp/pop2/{app,data,vol}
mv /tmp/pop2/vol /tmp/pop2/pop2
tar --format=ustar -czhf pop2-data.tar.gz -C /tmp/pop2 app data pop2
```

You now have `pop2-data.tar.gz` (~15 MB). Host it with a **direct link** — putting it
next to the engine is simplest (then there's no CORS to worry about).

---

## 3 · Tell the page where the data is

Any of these:

- **Bake it into the URL** (best for a page you control):
  `index.html?data=https://your-host/pop2-data.tar.gz`
  It fetches once and caches in the visitor's browser (IndexedDB) for next time.
- **Same-origin, short form** — if the archive sits next to the engine:
  `index.html?data=pop2-data.tar.gz`
- **Let the visitor choose** — with no `?data=`, the page shows a drop-zone: the
  visitor drops the file in, or pastes a direct link into the field.

> **CORS:** if the data is on a *different* host than the engine, that host must send
> `Access-Control-Allow-Origin`. Same-origin needs nothing.

---

## Embedding it in another page

Grab a snippet from `embed-code.html`, or write the `<iframe>` yourself:

```html
<iframe src="https://your-host/embed.html"
        width="640" height="480" loading="lazy"
        style="max-width:100%;aspect-ratio:4/3;border:0"
        allow="gamepad; fullscreen; autoplay"
        title="Prince of Persia 2 (Macintosh)"></iframe>
```

- `embed.html` is a light poster; the engine loads only when the visitor clicks Play.
- For a chromeless canvas straight away, embed `?embed=1` (and add `&data=…` since this
  engine has no baked-in data):
  `https://your-host/?embed=1&data=https://your-host/pop2-data.tar.gz`

---

## Notes

- Nothing is uploaded from the visitor — the data stays in their browser. The in-game
  menu's **Forget data** clears the cached copy (and re-fetches from `?data=` next time).
- The game itself is © its rights holders; this project ships only the engine and tools.
  Please don't redistribute the game data unless you have the right to.
- Questions or trouble embedding? Open an issue on the repo — happy to help.
