#include "core/web_page.h"

namespace pons {

namespace {

// Split in parts: MSVC limits a single string literal to about 16 KB.
constexpr const char* kPageParts[] = {
R"PONS(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="color-scheme" content="light dark">
<meta name="referrer" content="no-referrer">
<title>Pons — WiFi Transfer</title>
<style>
:root {
  --bg: #f4f6f8; --card: #ffffff; --text: #1a1d21; --muted: #666e78; --line: #e3e7eb;
  --accent: #0f8b8d; --accent-text: #ffffff; --soft: #e6f4f4; --danger: #c0392b; --ok: #1e8e3e;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #15181b; --card: #1f2327; --text: #eef1f3; --muted: #9aa3ac; --line: #2e3439;
    --accent: #3cc3c3; --accent-text: #0b1d1d; --soft: #1b3132; --danger: #ff7b6b; --ok: #5fd07c;
  }
}
* { box-sizing: border-box; }
body {
  margin: 0; background: var(--bg); color: var(--text);
  font: 16px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
  -webkit-text-size-adjust: 100%;
  padding: env(safe-area-inset-top) 0 env(safe-area-inset-bottom);
}
main { max-width: 560px; margin: 0 auto; padding: 16px; }
header { display: flex; align-items: center; gap: 12px; margin: 4px 0 16px; }
.logo { width: 40px; height: 40px; border-radius: 10px; background: var(--accent); display: grid; place-items: center; flex: none; }
.logo svg { width: 26px; height: 26px; }
h1 { font-size: 20px; margin: 0; }
.status { font-size: 13px; color: var(--muted); display: flex; align-items: center; gap: 6px; }
.dot { width: 8px; height: 8px; border-radius: 50%; background: var(--ok); }
.offline .dot { background: var(--danger); }
section { background: var(--card); border: 1px solid var(--line); border-radius: 16px; padding: 16px; margin-bottom: 14px; }
h2 { font-size: 15px; margin: 0 0 12px; display: flex; justify-content: space-between; align-items: baseline; }
h2 small { font-weight: normal; color: var(--muted); font-size: 13px; }
.empty { color: var(--muted); font-size: 14px; margin: 4px 0; }
ul { list-style: none; margin: 0; padding: 0; }
li { display: flex; align-items: center; gap: 12px; padding: 10px 0; border-top: 1px solid var(--line); }
li:first-child { border-top: 0; padding-top: 0; }
.info { flex: 1; min-width: 0; }
.name { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-size: 15px; }
.meta { font-size: 12px; color: var(--muted); }
.meta.error { color: var(--danger); }
.meta.done { color: var(--ok); }
a.button, label.button, button {
  -webkit-appearance: none; appearance: none; border: 0; border-radius: 10px; font: inherit; font-size: 15px;
  font-weight: 600; padding: 10px 16px; text-decoration: none; display: inline-flex; align-items: center;
  justify-content: center; gap: 8px; cursor: pointer; min-height: 44px;
}
.primary { background: var(--accent); color: var(--accent-text); }
.secondary { background: var(--soft); color: var(--text); }
.wide { width: 100%; }
button:disabled { opacity: .5; }
.bar { height: 6px; background: var(--line); border-radius: 3px; overflow: hidden; margin-top: 6px; }
.bar > div { height: 100%; width: 0; background: var(--accent); transition: width .15s; }
textarea {
  width: 100%; min-height: 96px; resize: vertical; border: 1px solid var(--line); border-radius: 10px;
  background: var(--bg); color: var(--text); font: inherit; padding: 10px; margin-bottom: 10px;
}
.row { display: flex; gap: 10px; }
.row > * { flex: 1; }
.note { font-size: 12px; color: var(--muted); margin-top: 10px; text-align: center; }
#ended { display: none; text-align: center; }
.ended #ended { display: block; }
.ended .live { display: none; }
input[type=file] { display: none; }
</style>
</head>
)PONS",
R"PONS(<body>
<main>
<header>
  <div class="logo"><svg viewBox="0 0 24 24" fill="none" stroke="#fff" stroke-width="2.2" stroke-linecap="round">
    <path d="M2 17h20M4 17v-3a8 8 0 0 1 16 0v3M8 17v-4M12 17v-6M16 17v-4"/></svg></div>
  <div>
    <h1>Pons</h1>
    <div class="status" id="status"><span class="dot"></span><span id="statusText">Connected to your PC</span></div>
  </div>
</header>

<section id="ended">
  <h2>Session ended</h2>
  <p class="empty">Pons was closed or its link changed. Scan the QR code on your PC again.</p>
</section>

<section class="live">
  <h2>From your PC <small id="fileCount"></small></h2>
  <p class="empty" id="noFiles">Nothing shared yet. Drag files into Pons on your PC and they show up here.</p>
  <ul id="files"></ul>
</section>

<section class="live">
  <h2>Send to your PC</h2>
  <label class="button primary wide" for="picker">
    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"><path d="M12 19V5M5 12l7-7 7 7"/></svg>
    Choose photos or files
  </label>
  <input type="file" id="picker" multiple>
  <ul id="uploads" style="margin-top:12px"></ul>
  <p class="note" id="folderNote">Files are saved on your PC.</p>
</section>

<section class="live">
  <h2>Shared text</h2>
  <textarea id="text" placeholder="Type or paste a link or some text…"></textarea>
  <div class="row">
    <button class="primary" id="sendText">Send to PC</button>
    <button class="secondary" id="copyText">Copy</button>
  </div>
</section>
</main>
)PONS",
R"PONS(<script>
"use strict";
const token = new URLSearchParams(location.search).get("t") || "";
const $ = (id) => document.getElementById(id);
let textRevision = -1;
let textDirty = false;
let ended = false;

function api(path) { return path + (path.includes("?") ? "&" : "?") + "t=" + encodeURIComponent(token); }

function formatSize(bytes) {
  const units = ["B", "KB", "MB", "GB", "TB"];
  let value = bytes, unit = 0;
  while (value >= 1024 && unit < units.length - 1) { value /= 1024; unit++; }
  return unit === 0 ? bytes + " B" : value.toFixed(value < 10 ? 1 : 0) + " " + units[unit];
}

function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function setOnline(online) {
  $("status").classList.toggle("offline", !online);
  $("statusText").textContent = online ? "Connected to your PC" : "Trying to reach your PC…";
}

function endSession() {
  ended = true;
  document.body.classList.add("ended");
  $("status").classList.add("offline");
  $("statusText").textContent = "Disconnected";
}

let shownFiles = "";
function renderFiles(files) {
  const key = JSON.stringify(files);
  if (key === shownFiles) return;
  shownFiles = key;
  const list = $("files");
  list.textContent = "";
  $("noFiles").style.display = files.length ? "none" : "";
  $("fileCount").textContent = files.length ? files.length + (files.length === 1 ? " file" : " files") : "";
  for (const file of files) {
    const item = element("li");
    const info = element("div", "info");
    info.append(element("div", "name", file.name), element("div", "meta", formatSize(file.size)));
    const link = element("a", "button secondary", "Download");
    link.href = api("/f/" + file.id);
    link.setAttribute("download", file.name);
    item.append(info, link);
    list.append(item);
  }
}

async function refresh() {
  if (ended) return;
  try {
    const response = await fetch(api("/api/state"), { cache: "no-store" });
    if (response.status === 403) { endSession(); return; }
    const state = await response.json();
    setOnline(true);
    renderFiles(state.files);
    if (state.folder) $("folderNote").textContent = "Files are saved in the " + state.folder + " folder on your PC.";
    if (state.textRevision !== textRevision && !textDirty && document.activeElement !== $("text")) {
      $("text").value = state.text;
      textRevision = state.textRevision;
    }
  } catch (error) {
    setOnline(false);
  }
}
)PONS",
R"PONS(
const queue = [];
let uploading = false;

$("picker").addEventListener("change", () => {
  for (const file of $("picker").files) {
    const item = element("li");
    const info = element("div", "info");
    const meta = element("div", "meta", "Waiting… · " + formatSize(file.size));
    const bar = element("div", "bar");
    const fill = element("div");
    bar.append(fill);
    info.append(element("div", "name", file.name), meta, bar);
    item.append(info);
    $("uploads").prepend(item);
    queue.push({ file, meta, fill, bar });
  }
  $("picker").value = "";
  next();
});

function next() {
  if (uploading || !queue.length || ended) return;
  uploading = true;
  const job = queue.shift();
  const request = new XMLHttpRequest();
  request.open("POST", api("/api/upload?name=" + encodeURIComponent(job.file.name)));
  request.upload.onprogress = (event) => {
    if (!event.lengthComputable) return;
    const percent = Math.round(event.loaded * 100 / event.total);
    job.fill.style.width = percent + "%";
    job.meta.textContent = percent + "% · " + formatSize(event.loaded) + " of " + formatSize(event.total);
  };
  request.onload = () => {
    if (request.status === 200) {
      let saved = job.file.name;
      try { saved = JSON.parse(request.responseText).name; } catch (e) {}
      job.fill.style.width = "100%";
      job.bar.remove();
      job.meta.textContent = "Saved as " + saved;
      job.meta.className = "meta done";
    } else {
      let message = "Could not send it";
      try { message = JSON.parse(request.responseText).error || message; } catch (e) {}
      fail(job, message);
      if (request.status === 403) endSession();
    }
    finish();
  };
  request.onerror = () => { fail(job, "Connection lost. Is Pons still open on your PC?"); finish(); };
  job.meta.textContent = "Sending…";
  request.send(job.file);
}

function fail(job, message) {
  job.bar.remove();
  job.meta.textContent = message;
  job.meta.className = "meta error";
}

function finish() { uploading = false; next(); }

$("text").addEventListener("input", () => { textDirty = true; });

$("sendText").addEventListener("click", async () => {
  const button = $("sendText");
  button.disabled = true;
  try {
    const response = await fetch(api("/api/text"), { method: "POST", body: $("text").value });
    if (response.status === 403) { endSession(); return; }
    const state = await response.json();
    textRevision = state.textRevision;
    textDirty = false;
    button.textContent = "Sent ✓";
    setTimeout(() => { button.textContent = "Send to PC"; }, 1500);
  } catch (error) {
    setOnline(false);
  } finally {
    button.disabled = false;
  }
});

$("copyText").addEventListener("click", async () => {
  const area = $("text");
  // The Clipboard API needs HTTPS; a local page falls back to a selection copy.
  try {
    await navigator.clipboard.writeText(area.value);
  } catch (error) {
    area.select();
    document.execCommand("copy");
    area.blur();
  }
  $("copyText").textContent = "Copied ✓";
  setTimeout(() => { $("copyText").textContent = "Copy"; }, 1500);
});

refresh();
setInterval(refresh, 2000);
document.addEventListener("visibilitychange", () => { if (!document.hidden) refresh(); });
</script>
</body>
</html>
)PONS",
};

}  // namespace

const std::string& PageHtml() {
    static const std::string page = [] {
        std::string html;
        for (const char* part : kPageParts) html += part;
        return html;
    }();
    return page;
}

const std::string& ExpiredPageHtml() {
    static const std::string page = R"PONS(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light dark"><title>Pons</title>
<style>body{font:16px/1.5 -apple-system,"Segoe UI",Roboto,sans-serif;max-width:480px;margin:15vh auto;padding:16px;text-align:center}</style>
</head><body><h1>Link expired</h1><p>Scan the QR code shown in Pons on your PC again.</p></body></html>
)PONS";
    return page;
}

}  // namespace pons
