"use strict";
const CATS = ["AUTH_FAILURE", "SECURITY_THREAT", "ERROR", "PERF_WARNING", "PII"];
const CAT_NAME = { AUTH_FAILURE: "Authentication failure", SECURITY_THREAT: "Security threat", ERROR: "Error", PERF_WARNING: "Performance warning", PII: "Personal data" };
const CAT_VAR = ["--auth", "--sec", "--err", "--perf", "--pii"];
const SEVS = ["INFO", "LOW", "MEDIUM", "HIGH", "CRITICAL"];
const SEV_ALPHA = [0.3, 0.45, 0.62, 0.82, 1];
const $ = (id) => document.getElementById(id);
const css = (v) => getComputedStyle(document.documentElement).getPropertyValue(v).trim();
const fmt = (n) => (n == null ? "—" : Math.round(n).toLocaleString("en-US"));
const fmt1 = (n) => (n == null ? "—" : n.toLocaleString("en-US", { maximumFractionDigits: 1 }));
const pt = (rows, f) => (rows || []).find((r) => Object.entries(f).every(([k, v]) => r[k] === v));
const REDUCED = window.matchMedia && matchMedia("(prefers-reduced-motion: reduce)").matches;
const hbHosts = new Set();      // chart hosts that keep elements between updates (cleared when the palette changes)
const SVGNS = "http://www.w3.org/2000/svg";
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

/* ------------------------------------------------------------------ small SVG chart toolkit */
function S(tag, attrs, text) {
  const e = document.createElementNS(SVGNS, tag);
  for (const k in attrs || {}) e.setAttribute(k, attrs[k]);
  if (text != null) e.textContent = text;
  return e;
}
function niceTicks(lo, hi, n = 5) {
  const span = hi - lo || 1, raw = span / n, p = Math.pow(10, Math.floor(Math.log10(raw)));
  const step = [1, 2, 2.5, 5, 10].map((m) => m * p).find((s) => s >= raw);
  const out = [];
  for (let v = Math.ceil(lo / step) * step; v <= hi + 1e-9; v += step) out.push(+v.toFixed(10));
  return out;
}
function shortNum(v) {
  const a = Math.abs(v);
  if (a >= 1e6) return +(v / 1e6).toFixed(2) + "M";
  if (a >= 1e3) return +(v / 1e3).toFixed(1).replace(/\.0$/, "") + "k";
  return +v.toFixed(2) + "";
}
function frame(host, w, h, m) {
  host.textContent = "";
  const svg = S("svg", { viewBox: `0 0 ${w} ${h}`, class: "chart", role: "img" });
  host.appendChild(svg);
  return { svg, x0: m.l, x1: w - m.r, y0: h - m.b, y1: m.t, w, h };
}
function yAxis(f, lo, hi, log, label, ticksOverride) {
  const ticks = ticksOverride || (log ? logTicks(lo, hi) : niceTicks(lo, hi));
  const sy = log ? (v) => f.y0 - ((Math.log10(Math.max(v, lo)) - Math.log10(lo)) / (Math.log10(hi) - Math.log10(lo))) * (f.y0 - f.y1)
                 : (v) => f.y0 - ((v - lo) / (hi - lo)) * (f.y0 - f.y1);
  for (const t of ticks) {
    f.svg.appendChild(S("line", { x1: f.x0, x2: f.x1, y1: sy(t), y2: sy(t), class: "grid" }));
    f.svg.appendChild(S("text", { x: f.x0 - 6, y: sy(t) + 4, "text-anchor": "end" }, shortNum(t)));
  }
  f.svg.appendChild(S("line", { x1: f.x0, x2: f.x0, y1: f.y1, y2: f.y0, class: "axisline" }));
  f.svg.appendChild(S("line", { x1: f.x0, x2: f.x1, y1: f.y0, y2: f.y0, class: "axisline" }));
  if (label) f.svg.appendChild(S("text", { x: 4, y: f.y1 - 8, "text-anchor": "start" }, label));
  return sy;
}
function logTicks(lo, hi) {
  const out = [];
  for (let e = Math.ceil(Math.log10(lo)); e <= Math.floor(Math.log10(hi)); e++) out.push(Math.pow(10, e));
  return out;
}
function keyFor(host, items) {
  host.textContent = "";
  for (const it of items) {
    const s = document.createElement("span");
    const sv = S("svg", { viewBox: "0 0 26 8" });
    if (it.fill) sv.appendChild(S("rect", { x: 0, y: 0, width: 26, height: 8, fill: it.color }));
    else sv.appendChild(S("line", { x1: 0, x2: 26, y1: 4, y2: 4, stroke: it.color, "stroke-width": 2.2, "stroke-dasharray": it.dash || "" }));
    s.appendChild(sv);
    s.appendChild(document.createTextNode(it.name));
    host.appendChild(s);
  }
}
/* series: [{name,color,dash,pts:[[x,y]]}]; xs: category positions (evenly spaced) */
function lineChart(host, series, o) {
  const f = frame(host, o.w || 560, o.h || 250, { l: 52, r: 16, t: 22, b: 36 });
  const xs = o.xs;
  let ys = series.flatMap((s) => s.pts.map((p) => p[1])).filter((v) => v != null && (!o.ylog || v > 0));
  let lo = o.ylog ? Math.pow(10, Math.floor(Math.log10(Math.min(...ys)))) : (o.ymin != null ? o.ymin : 0);
  let hi = o.ylog ? Math.pow(10, Math.ceil(Math.log10(Math.max(...ys)))) : Math.max(...ys) * 1.08;
  if (!o.ylog) { const t = niceTicks(lo, hi); hi = Math.max(hi, t[t.length - 1]); }
  const sy = yAxis(f, lo, hi, o.ylog, o.ylabel);
  const sx = o.xnum ? ((v) => f.x0 + ((v - xs[0]) / (xs[xs.length - 1] - xs[0] || 1)) * (f.x1 - f.x0))
                    : ((v) => f.x0 + (xs.indexOf(v) + 0.5) / xs.length * (f.x1 - f.x0));
  for (const x of xs) {
    f.svg.appendChild(S("text", { x: sx(x), y: f.y0 + 16, "text-anchor": "middle" }, o.xfmt ? o.xfmt(x) : x));
    f.svg.appendChild(S("line", { x1: sx(x), x2: sx(x), y1: f.y0, y2: f.y0 + 4, class: "axisline" }));
  }
  if (o.xlabel) f.svg.appendChild(S("text", { x: (f.x0 + f.x1) / 2, y: f.h - 4, "text-anchor": "middle" }, o.xlabel));
  if (o.ideal) {
    f.svg.appendChild(S("line", { x1: sx(xs[0]), y1: sy(xs[0]), x2: sx(xs[xs.length - 1]), y2: sy(Math.min(xs[xs.length - 1], hi)),
      stroke: css("--steel"), "stroke-width": 1, "stroke-dasharray": "2 4" }));
  }
  for (const s of series) {
    const pts = s.pts.filter((p) => p[1] != null && (!o.ylog || p[1] > 0));
    f.svg.appendChild(S("polyline", { points: pts.map((p) => `${sx(p[0])},${sy(p[1])}`).join(" "), fill: "none", stroke: s.color,
      "stroke-width": 2.2, "stroke-dasharray": s.dash || "", "stroke-linejoin": "round" }));
    for (const p of pts) {
      const c = S("circle", { cx: sx(p[0]), cy: sy(p[1]), r: 3.4, fill: s.color });
      c.appendChild(S("title", {}, `${s.name}: ${shortNum(p[1])}`));
      f.svg.appendChild(c);
    }
  }
  if (o.key) keyFor(o.key, series.map((s) => ({ name: s.name, color: s.color, dash: s.dash })));
}
/* rows: [{label,value,lo,hi,color}]. Called again with the same labels it updates the bars in place, so they glide. */
function hbars(host, rows, o = {}) {
  const w = o.w || 560, rowH = o.rowH || 30, m = { l: o.labelW || 150, r: 70, t: 8, b: o.log ? 28 : 8 };
  const h = m.t + m.b + rows.length * rowH;
  const x0 = m.l, x1 = w - m.r, y1 = m.t, y0 = h - m.b;
  const vals = rows.map((r) => r.hi != null ? r.hi : r.value);
  const lo = o.log ? Math.pow(10, Math.floor(Math.log10(Math.min(...rows.map((r) => r.value).filter((v) => v > 0))))) : 0;
  const hi = o.log ? Math.pow(10, Math.ceil(Math.log10(Math.max(...vals)))) : (o.max || Math.max(...vals, 0.0001) * 1.05);
  const sx = o.log ? (v) => x0 + ((Math.log10(Math.max(v, lo)) - Math.log10(lo)) / (Math.log10(hi) - Math.log10(lo))) * (x1 - x0)
                   : (v) => x0 + (v / hi) * (x1 - x0);
  const sig = [rows.map((r) => r.label).join("|"), o.log ? 1 : 0, w, rowH, o.labelW || 150, o.log ? hi : 0].join("#");
  const label = (r) => (o.fmt ? o.fmt(r.value) : shortNum(r.value));
  if (host.__hb && host.__hb.sig === sig) {
    rows.forEach((r, i) => {
      const e = host.__hb.els[i];
      e.bar.style.width = Math.max(1, sx(r.value) - x0) + "px";
      e.bar.setAttribute("fill", r.color || css("--ink"));
      e.val.style.transform = `translateX(${Math.min(sx(r.value) + 6, x1 + 4) - x0}px)`;
      e.val.textContent = label(r);
      e.tip.textContent = `${r.label}: ${fmt(r.value)}`;
    });
    return;
  }
  const f = frame(host, w, h, m);
  if (o.log) for (const t of logTicks(lo, hi)) {
    f.svg.appendChild(S("line", { x1: sx(t), x2: sx(t), y1, y2: y0, class: "grid" }));
    f.svg.appendChild(S("text", { x: sx(t), y: y0 + 16, "text-anchor": "middle" }, shortNum(t)));
  }
  f.svg.appendChild(S("line", { x1: x0, x2: x0, y1, y2: y0, class: "axisline" }));
  const els = [];
  rows.forEach((r, i) => {
    const y = y1 + i * rowH + rowH * 0.18, bh = rowH * 0.64;
    f.svg.appendChild(S("text", { x: x0 - 8, y: y + bh * 0.72, "text-anchor": "end", class: "lab" }, r.label));
    const bar = S("rect", { x: x0, y, width: Math.max(1, sx(r.value) - x0), height: bh, fill: r.color || css("--ink"), class: "bar" });
    const tip = S("title", {}, `${r.label}: ${fmt(r.value)}`);
    bar.appendChild(tip);
    f.svg.appendChild(bar);
    if (r.lo != null) f.svg.appendChild(S("line", { x1: sx(r.lo), x2: sx(r.hi), y1: y + bh / 2, y2: y + bh / 2, stroke: css("--bg"), "stroke-width": 1.6 }));
    const val = S("text", { x: x0, y: y + bh * 0.72, class: "lab val" }, label(r));
    val.style.transform = `translateX(${Math.min(sx(r.value) + 6, x1 + 4) - x0}px)`;
    f.svg.appendChild(val);
    els.push({ bar, val, tip });
  });
  host.__hb = { sig, els };
  hbHosts.add(host);
}
/* groups: [{label, vals:[...]}]; series:[{name,color}] */
function groupedBars(host, groups, series, o = {}) {
  const f = frame(host, o.w || 560, o.h || 240, { l: 40, r: 10, t: 20, b: 40 });
  const sy = yAxis(f, 0, o.max || 1, false, o.ylabel, o.ticks || [0, 0.25, 0.5, 0.75, 1]);
  const gw = (f.x1 - f.x0) / groups.length, bw = Math.min(26, (gw * 0.78) / series.length);
  groups.forEach((g, gi) => {
    const x0 = f.x0 + gi * gw + (gw - bw * series.length) / 2;
    g.vals.forEach((v, si) => {
      if (v == null) { f.svg.appendChild(S("text", { x: x0 + si * bw + bw / 2, y: f.y0 - 4, "text-anchor": "middle" }, "n/a")); return; }
      const r = S("rect", { x: x0 + si * bw, y: sy(v), width: bw - 2, height: Math.max(0.5, f.y0 - sy(v)), fill: series[si].color });
      r.appendChild(S("title", {}, `${g.label}, ${series[si].name}: ${v.toFixed(3)}`));
      f.svg.appendChild(r);
    });
    f.svg.appendChild(S("text", { x: f.x0 + gi * gw + gw / 2, y: f.y0 + 16, "text-anchor": "middle", class: "lab" }, g.label));
  });
  if (o.key) keyFor(o.key, series.map((s) => ({ name: s.name, color: s.color, fill: true })));
}

/* ------------------------------------------------------------------ tabs and controls */
const views = { live: $("v-live"), exp: $("v-exp"), acc: $("v-acc") };
document.querySelectorAll("nav button").forEach((b) => b.addEventListener("click", () => {
  document.querySelectorAll("nav button").forEach((x) => x.setAttribute("aria-selected", x === b));
  for (const k in views) { views[k].hidden = k !== b.dataset.view; views[k].classList.remove("switching"); }
  void views[b.dataset.view].offsetWidth; views[b.dataset.view].classList.add("switching");        // fade only when a person switches views
  $("controls").style.display = b.dataset.view === "live" ? "" : "none";
  if (b.dataset.view !== "live") loadResults();
  else if (last) renderLive(last);
}));
const rateOf = (v) => Math.round(Math.pow(10, 1 + v * 0.035));
const rate = $("rate");
const updRate = () => { $("rateOut").textContent = fmt(rateOf(+rate.value)) + " lines/s"; };
rate.addEventListener("input", updRate); updRate();
fetch("/api/sources").then((r) => r.json()).then((s) => {
  const sel = $("src");
  for (const k in s) { const o = document.createElement("option"); o.value = k; o.textContent = s[k].label; o.title = s[k].note; sel.appendChild(o); }
});
async function post(path, body) {
  const r = await fetch(path, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body || {}) });
  const j = await r.json();
  if (!r.ok) alert(j.error || "request failed");
  return j;
}
$("startBtn").addEventListener("click", () => post("/api/run", { source: $("src").value, rate: rateOf(+rate.value), n: 2000000 }));
$("stopBtn").addEventListener("click", () => post("/api/stop"));

/* ------------------------------------------------------------------ live view */
let last = null;
let lastAt = 0;                         // performance.now() when `last` arrived
let resultsLoaded = 0, lastResults = null;
const WINDOW_S = 180;
const serverNow = () => (last ? last.now_f + (performance.now() - lastAt) / 1000 : Date.now() / 1000);
function buildRails() {
  const host = $("rails");
  host.textContent = "";
  CATS.forEach((c, i) => {
    const d = document.createElement("div");
    d.className = "rail";
    d.innerHTML = `<div class="name"><b><i style="background:var(${CAT_VAR[i]})"></i>${CAT_NAME[c]}</b><span id="rt${i}">0 lines</span></div><canvas id="rc${i}" role="img" aria-label="${CAT_NAME[c]} events over the last three minutes"></canvas>`;
    host.appendChild(d);
  });
  $("sevLegend").innerHTML = "<span>Severity:</span>" + SEVS.map((s, i) => `<span><i style="opacity:${SEV_ALPHA[i]}"></i>${s.toLowerCase()}</span>`).join("");
}
function setup(cv) {
  const dpr = window.devicePixelRatio || 1, W = cv.clientWidth, H = cv.clientHeight;
  if (!W) return null;
  if (cv.width !== Math.round(W * dpr) || cv.height !== Math.round(H * dpr)) { cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr); }
  const g = cv.getContext("2d");
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, W, H);
  return { g, W, H };
}
/* Both charts are drawn against the clock, not against the snapshot: a bar for second t sits at (t - (now - 180)) / 180
 * of the width, so the picture slides left continuously between the one-second updates. */
function drawRail(i, tNow) {
  const c = setup($("rc" + i));
  if (!c || !last) return;
  const { g, W, H } = c, t0 = tNow - WINDOW_S, px = W / WINDOW_S, col = css(CAT_VAR[i]);
  g.strokeStyle = css("--faint"); g.lineWidth = 1;
  g.beginPath(); g.moveTo(0, H - 0.5); g.lineTo(W, H - 0.5); g.stroke();
  for (let t = Math.ceil(t0 / 60) * 60; t <= tNow; t += 60) { const x = Math.round((t - t0) * px) + 0.5; g.beginPath(); g.moveTo(x, 0); g.lineTo(x, H); g.stroke(); }
  let mx = 1;
  for (const s of last.series) mx = Math.max(mx, s[2][i]);
  g.fillStyle = col;
  for (const s of last.series) {
    const n = s[2][i];
    if (!n) continue;
    const xa = (s[0] - t0) * px, xb = (Math.min(s[0] + 1, tNow) - t0) * px;
    if (xb < 0) continue;
    const h = Math.max(3, Math.sqrt(n / mx) * (H - 4));
    g.globalAlpha = SEV_ALPHA[s[3][i]];
    g.fillRect(xa, H - h, Math.max(1.4, xb - xa - 0.5), h);
  }
  g.globalAlpha = 1;
}
let thrMax = 10;
function drawThroughput(tNow) {
  const c = setup($("thrCanvas"));
  if (!c || !last) return;
  const { g, W, H } = c, L = 40, B = 20, T = 8, t0 = tNow - WINDOW_S, px = (W - L - 6) / WINDOW_S;
  const pts = last.series.filter((s) => s[0] + 1 <= tNow);
  const want = Math.max(10, ...pts.map((s) => s[1])) * 1.1;
  thrMax = REDUCED || !drawThroughput.seen ? want : thrMax + (want - thrMax) * 0.08;
  drawThroughput.seen = true;                 // the scale eases, so the line does not jump when the peak changes
  const sy = (v) => H - B - (v / thrMax) * (H - B - T), ink = css("--ink"), steel = css("--steel");
  g.font = "11px " + getComputedStyle(document.body).fontFamily; g.textAlign = "right"; g.fillStyle = steel;
  for (const t of niceTicks(0, thrMax, 4)) {
    g.strokeStyle = css("--faint"); g.beginPath(); g.moveTo(L, sy(t) + 0.5); g.lineTo(W, sy(t) + 0.5); g.stroke();
    g.fillText(shortNum(t), L - 6, sy(t) + 4);
  }
  g.strokeStyle = ink; g.beginPath(); g.moveTo(L, T); g.lineTo(L, H - B + 0.5); g.lineTo(W, H - B + 0.5); g.stroke();
  g.textAlign = "center"; g.fillStyle = steel;
  [["3 min ago", L + 18], ["now", W - 14]].forEach(([tx, x]) => g.fillText(tx, x, H - 5));
  if (pts.length < 2) return;
  g.save(); g.beginPath(); g.rect(L, 0, W - L, H); g.clip();
  g.beginPath();
  pts.forEach((s, k) => { const x = L + (s[0] + 0.5 - t0) * px, y = sy(s[1]); k ? g.lineTo(x, y) : g.moveTo(x, y); });
  g.lineJoin = "round"; g.lineWidth = 2; g.strokeStyle = ink; g.stroke();
  const xl = L + (pts[pts.length - 1][0] + 0.5 - t0) * px, xf = L + (pts[0][0] + 0.5 - t0) * px;
  g.lineTo(xl, H - B); g.lineTo(xf, H - B); g.closePath(); g.globalAlpha = 0.1; g.fillStyle = ink; g.fill(); g.globalAlpha = 1;
  g.restore();
}
/* numbers ease toward their new value instead of jumping */
const tweens = new Set();
function tweenNum(el, target, html) {
  if (REDUCED || !el.__shown) { el.__v = target; el.__shown = true; el.innerHTML = html(target); tweens.delete(el); return; }
  el.__target = target; el.__html = html; tweens.add(el);
}
function stepTweens() {
  for (const el of tweens) {
    const d = el.__target - el.__v;
    if (Math.abs(d) < 0.5) { el.__v = el.__target; tweens.delete(el); } else el.__v += d * 0.2;
    el.innerHTML = el.__html(el.__v);
  }
}
function frameLoop() {
  if (last && !views.live.hidden) { stepTweens(); const t = serverNow(); CATS.forEach((c, i) => drawRail(i, t)); drawThroughput(t); }
  requestAnimationFrame(frameLoop);
}
function chipsOf(r) {
  const l = r.labels.map((x) => `<span class="chip ${x}">${CAT_NAME[x]}</span>`).join("");
  const f = r.flags.map((x) => `<span class="chip flag">${esc(x.replace(/_/g, " ").toLowerCase())}</span>`).join("");
  return l + f;
}
function feedItem(r) {
  const conf = r.primary ? `<span>conf ${r.conf.toFixed(2)}</span>` : "";
  return `<li class="sev-${r.sev}" data-seq="${r.seq}"><div class="meta"><b>${r.sev.toLowerCase()}</b><span>#${r.seq}</span>${conf}</div>` +
    `<div class="txt">${chipsOf(r)}<div>${esc(r.line)}</div></div></li>`;
}
/* Only lines that are new are inserted (and highlighted for a moment); the rest of the list stays where it is. */
function syncFeed(ul, items, emptyHtml) {
  if (!items.length) { ul.innerHTML = emptyHtml; ul.__seqs = new Set(); ul.__init = false; return; }
  if (ul.querySelector(".empty")) ul.innerHTML = "";
  const have = ul.__seqs || (ul.__seqs = new Set());
  const keys = new Set(items.map((r) => r.seq));
  const fresh = items.filter((r) => !have.has(r.seq)).reverse();
  for (const r of fresh) {
    const t = document.createElement("template");
    t.innerHTML = feedItem(r).trim();
    const li = t.content.firstChild;
    if (ul.__init && !REDUCED) li.classList.add("enter");
    ul.prepend(li);
    have.add(r.seq);
  }
  for (const li of [...ul.children]) if (!keys.has(+li.dataset.seq)) { have.delete(+li.dataset.seq); li.remove(); }
  ul.__init = true;
}
let prevTotal = 0;
function renderLive(s) {
  if (s.total < prevTotal) for (const id of ["feed", "alertFeed"]) { $(id).innerHTML = ""; $(id).__seqs = new Set(); $(id).__init = false; }
  prevTotal = s.total;
  const esc_n = Object.entries(s.flags).filter(([k]) => k.startsWith("ESCALATED") || k === "ERROR_STORM").reduce((a, [, v]) => a + v, 0);
  tweenNum($("kTotal"), s.total, (v) => fmt(v));
  tweenNum($("kRate"), s.rate, (v) => fmt(v) + "<small>lines/s</small>");
  tweenNum($("kAlerts"), s.alerts, (v) => fmt(v));
  tweenNum($("kEsc"), esc_n, (v) => fmt(v));
  tweenNum($("kPii"), s.pii, (v) => fmt(v));
  const st = $("status"), idle = s.idle_s;
  st.className = "status " + (s.running ? "live" : idle != null && idle < 4 ? "live" : s.total ? "stale" : "");
  $("statusTxt").textContent = s.running ? `Running: ${s.run.label}, ${fmt(s.run.rate)} lines/s`
    : s.total ? (idle < 4 ? "Receiving output" : "Run finished") : "Waiting for output in " + s.out;
  $("startBtn").disabled = s.running; $("stopBtn").disabled = !s.running;
  CATS.forEach((c, i) => tweenNum($("rt" + i), s.label[c], (v) => fmt(v) + " lines labelled"));
  syncFeed($("feed"), s.feed, `<li class="empty" style="display:block"><b>No output yet.</b> Press Start to run a paced source, or write a run into <code>${esc(s.out)}</code> with <code>bin/triage -o ${esc(s.out)} -</code>.</li>`);
  syncFeed($("alertFeed"), s.alert_feed, `<li class="empty" style="display:block">No alerts yet.</li>`);
  $("escNote").textContent = Object.entries(s.flags).map(([k, v]) => `${v} ${k.replace("ESCALATED_", "").replace(/_/g, " ").toLowerCase()}`).join(", ");

  hbars($("sevChart"), SEVS.map((v) => ({ label: v.toLowerCase(), value: s.sev[v], color: css("--ink") })), { w: 420, rowH: 24, labelW: 80, fmt: fmt });
  hbars($("mixChart"), CATS.map((c, i) => ({ label: CAT_NAME[c], value: s.cat[c], color: css(CAT_VAR[i]) })), { w: 420, rowH: 28, labelW: 150, fmt: fmt });
  const rules = s.rules.length ? s.rules : [["—", 0]];
  hbars($("ruleChart"), rules.map(([k, v]) => ({ label: k, value: v, color: css("--ink") })), { w: 420, rowH: 24, labelW: 130, fmt: fmt });
  confChart(s.conf_bins);
  const tn = serverNow(); CATS.forEach((c, i) => drawRail(i, tn)); drawThroughput(tn);      // draw now; the frame loop keeps them sliding
}
function confChart(bins) {
  const host = $("confChart"), mx = Math.max(1, ...bins);
  const ticks = niceTicks(0, mx * 1.05), top = ticks[ticks.length - 1] || mx;
  if (host.__cc && host.__cc.top === top) { host.__cc.rects.forEach((r, i) => { r.style.transform = `scaleY(${Math.min(1, bins[i] / top)})`; }); return; }
  const f = frame(host, 420, 200, { l: 44, r: 10, t: 14, b: 34 });
  yAxis(f, 0, top, false, "");
  const bw = (f.x1 - f.x0) / 10, rects = [];
  bins.forEach((v, i) => {
    const r = S("rect", { x: f.x0 + i * bw + 2, y: f.y1, width: bw - 4, height: f.y0 - f.y1, fill: css("--ink"), opacity: 0.35 + 0.065 * i, class: "cbar" });
    r.style.transform = `scaleY(${Math.min(1, v / top)})`;
    f.svg.appendChild(r); rects.push(r);
    f.svg.appendChild(S("text", { x: f.x0 + i * bw + bw / 2, y: f.y0 + 15, "text-anchor": "middle" }, (i / 10).toFixed(1)));
  });
  host.__cc = { top, rects };
  hbHosts.add(host);
}
buildRails();
function connect() {
  const es = new EventSource("/events");
  es.onmessage = (e) => { last = JSON.parse(e.data); lastAt = performance.now(); if (!views.live.hidden) renderLive(last); };
  es.onerror = () => { $("statusTxt").textContent = "Server not reachable, retrying"; $("status").className = "status"; };
}
/* ------------------------------------------------------------------ palettes */
const V = (bg, panel, ink, steel, rule, faint, auth, sec, err, perf, pii, chipink) =>
  ({ "--bg": bg, "--panel": panel, "--ink": ink, "--steel": steel, "--rule": rule, "--faint": faint, "--auth": auth, "--sec": sec, "--err": err,
     "--perf": perf, "--pii": pii, "--chipink": chipink, "--ok": perf, "--warn": pii, "--bad": sec });
const PALETTES = [
  { id: "harbour", name: "Harbour", note: "Cool mist and ink with blue, red, violet, teal and amber. The default.",
    light: V("#e9eef2", "#f6f8fa", "#12202b", "#5c6e7e", "#c9d3db", "#dde4ea", "#2159c9", "#b8302b", "#7446b8", "#0b7f70", "#b9710f", "#ffffff"),
    dark: V("#0f1a23", "#16232e", "#e4ecf2", "#8fa3b3", "#2a3c4a", "#1f2f3b", "#6f9bff", "#ff7d76", "#b794f4", "#4fd1bd", "#f0b45a", "#0b1118") },
  { id: "accessible", name: "Colour-blind safe", note: "Okabe and Ito hues, chosen to stay apart for the common kinds of colour blindness.",
    light: V("#f1f2f4", "#ffffff", "#1a1a1a", "#5f6368", "#d5d8dc", "#e8eaed", "#0072b2", "#c2510a", "#b4488a", "#00855f", "#9a6b00", "#ffffff"),
    dark: V("#12151a", "#1b1f26", "#eceff3", "#9aa3ad", "#2b323b", "#232930", "#56b4e9", "#f08a4b", "#e39cc2", "#2fc9a1", "#f0c040", "#0b1118") },
  { id: "dusk", name: "Dusk", note: "Indigo and plum with jewel tones. Easy on the eyes in a dark room.",
    light: V("#eceaf7", "#f8f7fd", "#1e1a3a", "#625e8a", "#d3cfec", "#e1def2", "#3b5bdb", "#c2255c", "#7048e8", "#0c8f7f", "#b45f06", "#ffffff"),
    dark: V("#15122a", "#1f1b38", "#ece9ff", "#a39fcf", "#342e5a", "#2a2548", "#7aa2ff", "#ff7a90", "#c792ff", "#5fe0c4", "#ffc66d", "#12102a") },
  { id: "moss", name: "Moss and graphite", note: "Green-grey paper and graphite ink. Quiet, with natural hues.",
    light: V("#e7ece6", "#f4f7f3", "#1b261f", "#5a6b5f", "#c5d0c6", "#d8e0d8", "#1f6fb2", "#b23a2e", "#8a4fa0", "#1f8a5f", "#a87315", "#ffffff"),
    dark: V("#121a16", "#19241f", "#e3ece5", "#8fa597", "#2b3b32", "#202e27", "#6aaee8", "#f08072", "#c59ad8", "#5fd6a0", "#e6b85c", "#0d1411") },
  { id: "ink", name: "Black and white", note: "Greys only, for printing or screenshots in a paper. Categories are told apart by rail and label.",
    light: V("#efefef", "#ffffff", "#000000", "#555555", "#bdbdbd", "#dddddd", "#000000", "#2e2e2e", "#555555", "#777777", "#8a8a8a", "#ffffff"),
    dark: V("#0c0c0c", "#161616", "#f2f2f2", "#a0a0a0", "#333333", "#222222", "#f2f2f2", "#d0d0d0", "#aaaaaa", "#888888", "#6a6a6a", "#000000") },
];
const qs = new URLSearchParams(location.search);
if (qs.get("still")) document.documentElement.classList.add("still");
const store = { get(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }, set(k, v) { try { localStorage.setItem(k, v); } catch (e) { /* private mode */ } } };
let palId = qs.get("theme") || store.get("triage.palette") || "harbour";
let modePref = qs.get("mode") || store.get("triage.mode") || "light";      // light by default (the professor asked for a light theme)
const dark = matchMedia("(prefers-color-scheme: dark)");
const curMode = () => (modePref === "auto" ? (dark.matches ? "dark" : "light") : modePref);
function applyPalette() {
  const p = PALETTES.find((x) => x.id === palId) || PALETTES[0], m = curMode(), root = document.documentElement;
  for (const [k, v] of Object.entries(p[m])) root.style.setProperty(k, v);
  root.style.colorScheme = m;
  hbHosts.forEach((h) => { h.__hb = null; h.__cc = null; });
  try { if (last) renderLive(last); if (lastResults) { renderExperiments(lastResults); renderAccuracy(lastResults); } } catch (e) { console.error(e); }
  buildPalettePanel();
}
function buildPalettePanel() {
  const panel = $("palPanel"), m = curMode();
  panel.innerHTML = `<h3>Palette</h3><div class="seg" role="group" aria-label="Light or dark">` +
    ["auto", "light", "dark"].map((k) => `<button data-mode="${k}" aria-pressed="${modePref === k}">${k[0].toUpperCase() + k.slice(1)}</button>`).join("") + `</div>` +
    PALETTES.map((p) => {
      const c = p[m];
      const dots = ["--bg", "--panel", "--ink", "--auth", "--sec", "--err", "--perf", "--pii"].map((k) => `<i style="background:${c[k]}"></i>`).join("");
      return `<button class="pal" data-id="${p.id}" aria-pressed="${p.id === palId}"><b>${p.name}</b><span class="note">${p.note}</span><span class="sw">${dots}</span></button>`;
    }).join("");
  panel.querySelectorAll("[data-mode]").forEach((b) => b.addEventListener("click", () => { modePref = b.dataset.mode; store.set("triage.mode", modePref); applyPalette(); }));
  panel.querySelectorAll(".pal").forEach((b) => b.addEventListener("click", () => { palId = b.dataset.id; store.set("triage.palette", palId); applyPalette(); }));
}
function togglePanel(open) {
  const panel = $("palPanel"), btn = $("palBtn");
  panel.classList.toggle("open", open); panel.setAttribute("aria-hidden", String(!open)); btn.setAttribute("aria-expanded", String(open));
}
$("palBtn").addEventListener("click", (e) => { e.stopPropagation(); togglePanel(!$("palPanel").classList.contains("open")); });
document.addEventListener("click", (e) => { if (!e.target.closest(".palette")) togglePanel(false); });
document.addEventListener("keydown", (e) => { if (e.key === "Escape") { togglePanel(false); $("palBtn").focus(); } });
dark.addEventListener && dark.addEventListener("change", () => { if (modePref === "auto") applyPalette(); });
{ const root = document.documentElement, p = PALETTES.find((x) => x.id === palId) || PALETTES[0];
  for (const [k, v] of Object.entries(p[curMode()])) root.style.setProperty(k, v);
  root.style.colorScheme = curMode(); }                 // first paint in the chosen palette
buildPalettePanel();

let boot = null;
try { boot = JSON.parse($("boot").textContent); } catch (e) { /* opened without the server page */ }
function guard(fn, what) {
  try { fn(); } catch (e) { console.error(what, e); $("statusTxt").textContent = `Could not draw ${what}: ${e.message}`; const n = document.createElement("p"); n.className = "warn"; n.textContent = `Could not draw the ${what}: ${e.message}`; document.querySelector("main").prepend(n); }
}
if (boot) { last = boot.state; lastAt = performance.now(); lastResults = boot.results; guard(() => renderLive(last), "live view"); guard(() => renderExperiments(boot.results), "experiments"); guard(() => renderAccuracy(boot.results), "accuracy"); resultsLoaded = Date.now(); }
else fetch("/api/state").then((r) => r.json()).then((j) => { last = j; lastAt = performance.now(); renderLive(j); });
connect();
const startTab = new URLSearchParams(location.search).get("tab") || (location.hash || "#live").slice(1);
const tabBtn = document.querySelector(`nav button[data-view="${startTab}"]`);
if (tabBtn) tabBtn.click();
if (qs.get("panel")) togglePanel(true);          // ?panel=1 opens the palette picker (after the tab click, which would close it)
window.addEventListener("resize", () => { if (last && !views.live.hidden) { hbHosts.forEach((h) => { h.__hb = null; h.__cc = null; }); renderLive(last); } });
requestAnimationFrame(frameLoop);

/* ------------------------------------------------------------------ experiments and accuracy */
async function loadResults() {
  if (Date.now() - resultsLoaded < 5000) return;
  resultsLoaded = Date.now();
  const R = await (await fetch("/api/results")).json();
  lastResults = R;
  renderExperiments(R);
  renderAccuracy(R);
}
function renderExperiments(R) {
  const ink = css("--ink"), steel = css("--steel");
  $("machineNote").textContent = R.machine ? `Measured on ${R.machine}. Every point is the median of five runs (bars show min to max); all numbers come from files under results/.` : "";
  if (R.e1) {
    const names = { "c1-fork": "Paper's design (fork)", "c2-posix-w1": "Library regexec, 1 worker" };
    hbars($("e1"), R.e1.map((r) => ({ label: names[r.id] || (r.engine === "posix" ? `Library regexec, ${r.workers} workers` : `DFA engine, ${r.workers} workers`),
      value: r.lines_per_s.median, lo: r.lines_per_s.min, hi: r.lines_per_s.max, color: r.engine === "dfa" ? ink : r.engine === "fork" ? css("--sec") : steel })),
      { log: true, labelW: 190, w: 600 });
  }
  if (R.e2) {
    const ws = [...new Set(R.e2.map((r) => r.workers))].sort((a, b) => a - b);
    const mk = (e) => ws.map((w) => [w, pt(R.e2, { engine: e, workers: w }).lines_per_s.median]);
    lineChart($("e2"), [{ name: "Library regexec", color: steel, dash: "6 4", pts: mk("posix") }, { name: "DFA engine", color: ink, pts: mk("dfa") }],
      { xs: ws, ylabel: "lines/s", xlabel: "workers", key: $("e2k"), ymin: 0 });
  }
  if (R.e3) {
    const ns = [5, 10, 20, 32, 64];
    const mk = (e) => ns.map((n) => [n, pt(R.e3, { engine: e, rules: "bench_" + n }).lines_per_s.median]);
    lineChart($("e3"), [{ name: "Library regexec", color: steel, dash: "6 4", pts: mk("posix") }, { name: "DFA engine", color: ink, pts: mk("dfa") }],
      { xs: ns, ylabel: "lines/s", xlabel: "rules in the set", key: $("e3k"), ymin: 0 });
    const rows = ns.map((n) => pt(R.e3, { engine: "dfa", rules: "bench_" + n }));
    $("e3s").innerHTML = `<table class="t"><tr><th>Rules</th><th>DFA groups</th><th>DFA states</th><th>Build (ms)</th></tr>` +
      rows.map((r, i) => `<tr><td>${ns[i]}</td><td>${r.dfa_groups}</td><td>${fmt(r.dfa_states)}</td><td>${fmt(r.build_ms)}</td></tr>`).join("") + "</table>";
  }
  if (R.e4) {
    const bs = R.e4.map((r) => r.batch).sort((a, b) => a - b);
    const get = (f) => bs.map((b) => [b, f(pt(R.e4, { batch: b }))]);
    lineChart($("e4"), [{ name: "Throughput (lines/s)", color: ink, pts: get((r) => r.lines_per_s.median) }],
      { xs: bs, ylabel: "lines/s", xlabel: "batch size (lines)", ymin: 0, key: $("e4k") });
  }
  const bp = R.basepaper || {};
  if (bp.rb2) {
    const variants = ["fork", "seq-recompile", "seq-precompiled", "threads-pattern", "threads-data_t5", "dfa_t5"];
    const names = { fork: "fork (the paper)", "seq-recompile": "sequential, recompile", "seq-precompiled": "sequential, precompiled", "threads-pattern": "threads per pattern", "threads-data_t5": "threads over inputs", dfa_t5: "DFA, threads over inputs" };
    const dashes = ["1 3", "6 4", "", "8 3 2 3", "", "2 2"];
    const cols = [css("--sec"), steel, steel, steel, ink, ink];
    const bs = [...new Set(bp.rb2.map((r) => r.batch))].sort((a, b) => a - b);
    lineChart($("rb2"), variants.map((v, i) => ({ name: names[v], color: cols[i], dash: dashes[i], pts: bs.map((b) => [b, (pt(bp.rb2, { variant: v, batch: b }) || {}).inputs_per_s?.median]) })),
      { xs: bs, ylog: true, xlabel: "inputs per batch", ylabel: "inputs/s", key: $("rb2k"), xfmt: shortNum });
  }
  if (bp.rb3) {
    const vs = ["fork", "threads-pattern", "threads-data", "dfa"];
    const names = { fork: "fork (the paper)", "threads-pattern": "threads per pattern", "threads-data": "threads over inputs", dfa: "DFA over inputs" };
    const dashes = ["1 3", "6 4", "", "2 2"];
    const cols = [css("--sec"), steel, ink, ink];
    const ks = [...new Set(bp.rb3.map((r) => r.k))].sort((a, b) => a - b);
    lineChart($("rb3"), vs.map((v, i) => ({ name: names[v], color: cols[i], dash: dashes[i], pts: ks.map((k) => [k, (pt(bp.rb3, { variant: v, k }) || {}).speedup_vs_same_variant_k1]) })),
      { xs: ks, ymin: 0, ylabel: "speed-up", xlabel: "cores", key: $("rb3k"), ideal: true, xnum: false });
  }
}
function renderAccuracy(R) {
  const ink = css("--ink"), steel = css("--steel");
  const short = { AUTH_FAILURE: "Auth", SECURITY_THREAT: "Threat", ERROR: "Error", PERF_WARNING: "Perf", PII: "PII" };
  const ser = [{ name: "Precision", color: steel }, { name: "Recall", color: ink }, { name: "F1", color: css("--auth") }];
  const prf = (res) => CATS.map((c) => ({ label: short[c], vals: ["precision", "recall", "f1"].map((k) => res.categories[c][k]) }));
  if (R.e6) {
    $("e6n").textContent = `${fmt(R.e6.n)} generated lines`;
    groupedBars($("e6"), prf(R.e6), ser, { key: $("e6k") });
  }
  if (R.e7 && R.e6 && R.e7.off) {
    const on = R.e6.categories.PII, off = R.e7.off.categories.PII;
    hbars($("e7"), [{ label: "Validators off", value: off.precision, color: steel }, { label: "Validators on", value: on.precision, color: ink }],
      { max: 1, w: 560, labelW: 130, rowH: 34, fmt: (v) => v.toFixed(2) });
    const L = R.e7.pii_on_logs;
    $("e7t").innerHTML = L ? `<table class="t"><tr><th>Real log</th><th>Lines</th><th>PII lines, off</th><th>on</th></tr>` +
      Object.entries(L).map(([k, v]) => `<tr><td>${esc(k.replace("_", " "))}</td><td>${fmt(v.lines)}</td><td>${fmt(v.lines_labelled_pii_no_validators)}</td><td>${fmt(v.lines_labelled_pii_validators)}</td></tr>`).join("") + "</table>" : "";
  }
  if (R.ai100) groupedBars($("ai"), prf(R.ai100), ser, { key: $("aik") });
  if (R.ait && Object.keys(R.ait).length) {
    hbars($("ait"), Object.entries(R.ait).map(([k, v]) => ({ label: k.replace("intranet_server__", "").replace("intranet.smith.russellmitchell.com-", "").slice(0, 22),
      value: v.recall || 0, color: ink })), { max: 1, w: 560, labelW: 170, rowH: 34, fmt: (v) => (v * 100).toFixed(2) + " %" });
  }
  if (R.loghub) {
    const names = Object.keys(R.loghub);
    const f = frame($("loghub"), 560, 40 + names.length * 34, { l: 90, r: 20, t: 6, b: 24 });
    names.forEach((n, i) => {
      const p = R.loghub[n].default.primary, tot = R.loghub[n].lines, y = f.y1 + i * 34 + 6;
      let x = f.x0;
      f.svg.appendChild(S("text", { x: f.x0 - 8, y: y + 14, "text-anchor": "end", class: "lab" }, n.replace("_2k", " 2k")));
      CATS.forEach((c, ci) => {
        const w = ((p[c] || 0) / tot) * (f.x1 - f.x0);
        if (w > 0) { const r = S("rect", { x, y, width: Math.max(w, 1), height: 20, fill: css(CAT_VAR[ci]) }); r.appendChild(S("title", {}, `${n}: ${CAT_NAME[c]} ${fmt(p[c])}`)); f.svg.appendChild(r); x += w; }
      });
      f.svg.appendChild(S("text", { x: f.x1 + 2, y: y + 14, class: "lab" }, ""));
    });
    f.svg.appendChild(S("text", { x: f.x0, y: f.h - 4 }, "Share of lines per primary label (unlabelled lines are the empty remainder). Hover for counts."));
  }
}
