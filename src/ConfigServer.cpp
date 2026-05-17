#include "ConfigServer.h"
#include <httplib.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

using json = nlohmann::json;
using namespace std::chrono;

static const char* INDEX_HTML = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MeshCore-Linux — Repeater Setup</title>
<style>
 :root {
   --bg:#0f1626; --panel:#171f33; --panel2:#1d2640; --line:#2a3450;
   --txt:#e7ecf7; --muted:#8c96ad; --acc:#7ec4ff; --ok:#7dd28b; --err:#ff7b72;
   --warn:#ffb347; --btn-bg:#3aa0e6; --btn-fg:#0a121f;
 }
 *{box-sizing:border-box}
 body{margin:0;padding:0 1em 2em;background:var(--bg);color:var(--txt);
      font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;font-size:15px;line-height:1.4}
 .wrap{max-width:960px;margin:0 auto;padding-top:1em}
 h1{font-size:1.45em;margin:.3em 0 .2em} h2{font-size:1.2em;margin:.3em 0 .2em} h3{font-size:1em;margin:0}
 .head{display:flex;align-items:flex-start;gap:.8em;margin:.6em 0 1.4em;padding-bottom:.6em;border-bottom:1px solid var(--line)}
 .head .meta{color:var(--muted);font-size:.88em}
 .step{display:flex;gap:.9em;align-items:flex-start;margin:1.2em 0}
 .step .num{flex:0 0 2em;width:2em;height:2em;border-radius:50%;border:1px solid var(--acc);
            color:var(--acc);display:flex;align-items:center;justify-content:center;font-weight:600}
 .step .body{flex:1 1 auto;min-width:0}
 .desc{color:var(--muted);font-size:.92em;margin:.2em 0 .8em}
 label{display:block;color:var(--muted);font-size:.85em;margin:.7em 0 .25em}
 input[type=text],input[type=number],input[type=password],select{
   width:100%;background:var(--panel);color:var(--txt);border:1px solid var(--line);
   border-radius:6px;padding:.6em .75em;font-size:.95em;font-family:inherit
 }
 input:focus,select:focus{outline:none;border-color:var(--acc)}
 .grid2{display:grid;grid-template-columns:1fr 1fr;gap:.8em}
 .row{display:flex;gap:.8em;align-items:center}
 .toggle{display:flex;align-items:center;justify-content:space-between;gap:1em;
         padding:.7em 0;border-top:1px solid var(--line)}
 .toggle:first-child{border:0}
 .toggle .copy{flex:1 1 auto}
 .toggle .copy .name{font-weight:600}
 .toggle .copy .desc{margin:.15em 0 0;font-size:.85em}
 .switch{position:relative;width:48px;height:26px;flex:0 0 auto}
 .switch input{opacity:0;width:0;height:0}
 .slider{position:absolute;cursor:pointer;inset:0;background:#3a4566;border-radius:26px;transition:.2s}
 .slider:before{position:absolute;content:"";height:20px;width:20px;left:3px;top:3px;background:#fff;border-radius:50%;transition:.2s}
 input:checked + .slider{background:var(--acc)}
 input:checked + .slider:before{transform:translateX(22px)}
 .btn{padding:.7em 1.1em;border:0;border-radius:8px;cursor:pointer;font-size:.95em;font-weight:600;font-family:inherit}
 .btn-primary{background:var(--btn-bg);color:var(--btn-fg)}
 .btn-secondary{background:transparent;color:var(--acc);border:1px solid var(--acc)}
 .btn-success{background:var(--ok);color:var(--btn-fg)}
 .btn-danger{background:#c4564a;color:#fff}
 .actions{display:flex;flex-wrap:wrap;gap:.6em;margin-top:1.4em}
 details{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:.4em .9em;margin-top:1em}
 details summary{cursor:pointer;color:var(--acc);font-weight:600;padding:.4em 0}
 details[open] summary{margin-bottom:.4em}
 .msg{margin-left:.6em;font-size:.9em}
 .msg.ok{color:var(--ok)} .msg.err{color:var(--err)} .msg.muted{color:var(--muted)}
 .status-box{background:var(--panel2);border:1px solid var(--line);border-radius:8px;
             padding:.6em .9em;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:.85em;
             white-space:pre-wrap;color:var(--muted);max-height:14em;overflow:auto}
 .term{background:#06090f;border:1px solid var(--line);border-radius:8px;padding:.6em .8em;
       font-family:ui-monospace,Menlo,Consolas,monospace;font-size:.88em;color:#cfe3ff;
       height:18em;overflow-y:scroll;white-space:pre-wrap;line-height:1.35}
 .term .cmd{color:#ffd479}
 .term .ans{color:#7dd28b}
 .term .err{color:#ff7b72}
 .term .sys{color:#8c96ad;font-style:italic}
 .term-input{display:flex;gap:.5em;margin-top:.5em}
 .term-input input{flex:1 1 auto;background:#06090f;border-color:var(--line);
                   font-family:ui-monospace,Menlo,Consolas,monospace}
 footer{margin-top:2.2em;color:var(--muted);font-size:.85em;text-align:center;border-top:1px solid var(--line);padding-top:.8em}
 @media (max-width:600px){ .grid2{grid-template-columns:1fr} .head{flex-direction:column} }
</style>
</head>
<body>
<div class="wrap">

<header class="head">
  <div style="flex:1 1 auto">
    <h1>Repeater Setup</h1>
    <div class="meta" id="meta">MeshCore-Linux &middot; loading&hellip;</div>
  </div>
  <button class="btn btn-secondary" onclick="refreshStatus()">Refresh</button>
</header>

<!-- ── Section 0: Radio Connection (TCP modem endpoint) ───────────────── -->
<div class="step">
  <div class="num">📡</div>
  <div class="body">
    <h2>Radio Connection</h2>
    <p class="desc">The TCP modem this repeater talks to. Must be running <a style="color:var(--acc)" href="https://github.com/itk80/pymc_usb">pymc_usb</a> firmware. Changing either field forces a clean reconnect (RST-on-close).</p>
    <div class="grid2">
      <div><label>Modem host (IP or hostname)</label><input type="text"  id="modem_host"></div>
      <div><label>Modem port</label>                  <input type="number" id="modem_port" min="1" max="65535"></div>
    </div>
    <label>Auth token (hex, empty for no auth)</label>
    <input type="text" id="modem_token" placeholder="00112233...">
  </div>
</div>

<!-- ── Section 1: Name & Location ─────────────────────────────────────── -->
<div class="step">
  <div class="num">1</div>
  <div class="body">
    <h2>Name &amp; Location</h2>
    <p class="desc">Give your repeater a name and set its location (decimal degrees).</p>
    <label>Name</label>
    <input type="text" id="node_name" placeholder="My Repeater">
    <div class="grid2" style="margin-top:.6em">
      <div><label>Latitude</label> <input type="number" step="0.0000001" id="node_lat" placeholder="0"></div>
      <div><label>Longitude</label><input type="number" step="0.0000001" id="node_lon" placeholder="0"></div>
    </div>
    <p class="desc" id="mapLink"></p>
  </div>
</div>

<!-- ── Section 2: Radio Settings ──────────────────────────────────────── -->
<div class="step">
  <div class="num">2</div>
  <div class="body">
    <h2>Radio Settings</h2>
    <p class="desc">Choose a regional preset or set parameters manually. All devices in your mesh must use the same settings.</p>
    <label>Regional Preset</label>
    <select id="lora_preset" onchange="onPresetChange()">
      <option value="EU/UK (Narrow)">EU/UK (Narrow)</option>
      <option value="EU/UK (Wide)">EU/UK (Wide)</option>
      <option value="US">US</option>
      <option value="Custom">Custom</option>
    </select>
    <p class="desc"><em>Choose <b>Custom</b> to edit the fields below manually.</em></p>
    <label>Frequency (MHz)</label>
    <input type="number" step="0.001" id="lora_freq_mhz">
    <label>Bandwidth (kHz)</label>
    <select id="lora_bw_khz">
      <option>7.8</option><option>10.4</option><option>15.6</option><option>20.8</option>
      <option>31.25</option><option>41.7</option><option>62.5</option><option>125</option>
      <option>250</option><option>500</option>
    </select>
    <label>Spreading Factor</label>
    <select id="lora_sf"><option>5</option><option>6</option><option>7</option><option>8</option>
                        <option>9</option><option>10</option><option>11</option><option>12</option></select>
    <label>Coding Rate</label>
    <select id="lora_cr"><option value="5">5 (4/5)</option><option value="6">6 (4/6)</option>
                        <option value="7">7 (4/7)</option><option value="8">8 (4/8)</option></select>
    <label>TX Power (dBm)</label>
    <input type="number" step="1" id="lora_tx_power_dbm">
    <label>Duty Cycle (%)</label>
    <input type="number" step="1" id="repeater_duty_cycle_pct">
  </div>
</div>

<!-- ── Section 3: Advertising ─────────────────────────────────────────── -->
<div class="step">
  <div class="num">3</div>
  <div class="body">
    <h2>Advertising</h2>
    <p class="desc">Control how often your device announces itself on the mesh network.</p>
    <label>Advert Interval (minutes)</label>
    <input type="number" step="1" id="repeater_advert_interval_min" min="0">
    <label>Flood Advert Interval (hours)</label>
    <input type="number" step="1" id="repeater_flood_advert_interval_h" min="0">
    <label>Flood Max Hops</label>
    <input type="number" step="1" id="repeater_flood_max_hops">
    <div class="toggle">
      <div class="copy">
        <div class="name">Large Mesh Optimisation</div>
        <div class="desc">Reduces unnecessary broadcasts by disabling repeater adverts. Recommended for busy networks.</div>
      </div>
      <label class="switch"><input type="checkbox" id="repeater_large_mesh_optim"><span class="slider"></span></label>
    </div>
  </div>
</div>

<!-- ── Section 4: Access Control ──────────────────────────────────────── -->
<div class="step">
  <div class="num">4</div>
  <div class="body">
    <h2>Access Control</h2>
    <p class="desc">Set passwords to control who can manage this device.</p>
    <label>Admin Password</label>
    <input type="password" id="node_admin_password" placeholder="Leave empty to keep current">
    <label>Guest Password</label>
    <input type="password" id="node_guest_password" placeholder="Leave empty to allow guest access">

    <details>
      <summary>Advanced Settings</summary>
      <label>Loop Detection</label>
      <select id="repeater_loop_detect">
        <option value="off">off</option>
        <option value="minimal">minimal</option>
        <option value="moderate">moderate</option>
        <option value="strict">strict</option>
      </select>
      <label>Path Hash Size</label>
      <select id="repeater_path_hash_mode">
        <option value="0">1 byte / hop  (smallest packets, most collisions)</option>
        <option value="1">2 bytes / hop (balanced — default)</option>
        <option value="2">3 bytes / hop (largest packets, fewest collisions)</option>
      </select>
      <label>Interference Threshold</label>
      <input type="number" id="repeater_interference_threshold">
      <label>AGC Reset Interval</label>
      <input type="number" id="repeater_agc_reset_interval">
      <label>RX Delay Base</label>
      <input type="number" step="0.01" id="repeater_rx_delay_base">
      <label>TX Delay Factor</label>
      <input type="number" step="0.01" id="repeater_tx_delay_factor">
      <label>Direct TX Delay</label>
      <input type="number" step="0.0000001" id="repeater_direct_tx_delay_factor">
      <div class="toggle">
        <div class="copy"><div class="name">Multi ACKs</div><div class="desc">Enable multi-ack support.</div></div>
        <label class="switch"><input type="checkbox" id="repeater_multi_acks"><span class="slider"></span></label>
      </div>
    </details>
  </div>
</div>

<!-- ── Actions ────────────────────────────────────────────────────────── -->
<div class="actions">
  <button class="btn btn-primary" onclick="save()">💾 Save Settings</button>
  <button class="btn btn-secondary" onclick="sendAdvert()">📡 Send Advert</button>
  <button class="btn btn-danger" onclick="restart()">⟳ Restart service</button>
  <span class="msg muted" id="saveMsg"></span>
</div>

<!-- ── Status panel ───────────────────────────────────────────────────── -->
<div class="step">
  <div class="num">📊</div>
  <div class="body">
    <h2>Status</h2>
    <div class="status-box" id="status">loading&hellip;</div>
  </div>
</div>

<!-- ── CLI Terminal ───────────────────────────────────────────────────── -->
<div class="step">
  <div class="num">▶</div>
  <div class="body">
    <h2>CLI Terminal</h2>
    <p class="desc">Send raw commands to the repeater (e.g. <code>get neighbors</code>, <code>set tx_power 14</code>, <code>advert</code>). Output is also visible in <code>journalctl -fu Meshcore-Linux</code>.</p>
    <div class="term" id="term"><span class="sys">[ready] try: get stats · get radio.stats · advert · ver</span>
</div>
    <div class="term-input">
      <input type="text" id="cli" placeholder="Command... (↑/↓ history, Enter to send)" autocomplete="off">
      <button class="btn btn-primary" onclick="cliSend()">Send</button>
    </div>
  </div>
</div>

<footer>MeshCore-Linux &middot; HTTP API <code>:8080</code> &middot; <a style="color:var(--acc)" href="/api/config">raw config</a> &middot; <a style="color:var(--acc)" href="/api/status">raw status</a></footer>

</div>

<script>
const PRESETS = {
  'EU/UK (Narrow)':{ freq_mhz:869.618, bw_khz:62.5, sf:8, cr:8 },
  'EU/UK (Wide)':  { freq_mhz:868.5,   bw_khz:250,  sf:8, cr:5 },
  'US':            { freq_mhz:915.0,   bw_khz:125,  sf:7, cr:5 },
};

let CFG = {};

function $(id){ return document.getElementById(id); }
function setVal(id, v){ const e=$(id); if(!e) return; if (e.type==='checkbox') e.checked = !!v; else e.value = (v===null||v===undefined)?'':v; }
function getVal(id){ const e=$(id); if(!e) return null; return e.type==='checkbox' ? e.checked : e.value; }

async function load(){
  try {
    const r = await fetch('/api/config'); CFG = await r.json();
  } catch(e){ alert('cannot fetch config: '+e); return; }

  setVal('modem_host',  CFG?.modem?.host);
  setVal('modem_port',  CFG?.modem?.port);
  setVal('modem_token', CFG?.modem?.token);

  setVal('node_name', CFG?.node?.name);
  setVal('node_lat',  CFG?.node?.lat);
  setVal('node_lon',  CFG?.node?.lon);
  setVal('node_admin_password', '');  // never prefill — leave empty = keep current
  setVal('node_guest_password', '');

  setVal('lora_preset', CFG?.lora?.preset || 'Custom');
  setVal('lora_freq_mhz', CFG?.lora?.freq_mhz);
  setVal('lora_bw_khz',   CFG?.lora?.bw_khz);
  setVal('lora_sf',       CFG?.lora?.sf);
  setVal('lora_cr',       CFG?.lora?.cr);
  setVal('lora_tx_power_dbm', CFG?.lora?.tx_power_dbm);

  const r = CFG?.repeater || {};
  setVal('repeater_duty_cycle_pct',          r.duty_cycle_pct ?? 50);
  setVal('repeater_advert_interval_min',     r.advert_interval_min ?? 0);
  setVal('repeater_flood_advert_interval_h', r.flood_advert_interval_h ?? 0);
  setVal('repeater_flood_max_hops',          r.flood_max_hops ?? 64);
  setVal('repeater_large_mesh_optim',        !!r.large_mesh_optim);
  setVal('repeater_loop_detect',             r.loop_detect || 'minimal');
  setVal('repeater_path_hash_mode',          r.path_hash_mode ?? 1);
  setVal('repeater_interference_threshold',  r.interference_threshold ?? 0);
  setVal('repeater_agc_reset_interval',      r.agc_reset_interval ?? 0);
  setVal('repeater_rx_delay_base',           r.rx_delay_base ?? 0);
  setVal('repeater_tx_delay_factor',         r.tx_delay_factor ?? 0.5);
  setVal('repeater_direct_tx_delay_factor',  r.direct_tx_delay_factor ?? 0.3);
  setVal('repeater_multi_acks',              !!r.multi_acks);

  updateMapLink();
}

function updateMapLink(){
  const lat = parseFloat(getVal('node_lat')); const lon = parseFloat(getVal('node_lon'));
  const link = $('mapLink');
  if (!isNaN(lat) && !isNaN(lon) && (lat!==0 || lon!==0)) {
    link.innerHTML = `📍 <a style="color:var(--acc)" target="_blank" href="https://www.openstreetmap.org/?mlat=${lat}&mlon=${lon}#map=15/${lat}/${lon}">Open on OpenStreetMap</a>`;
  } else { link.textContent = 'Set lat/lon to see a map link.'; }
}
$('node_lat').addEventListener('change', updateMapLink);
$('node_lon').addEventListener('change', updateMapLink);

function onPresetChange(){
  const name = getVal('lora_preset');
  const p = PRESETS[name];
  if (!p) return;   // "Custom" — leave fields alone
  setVal('lora_freq_mhz', p.freq_mhz);
  setVal('lora_bw_khz',   p.bw_khz);
  setVal('lora_sf',       p.sf);
  setVal('lora_cr',       p.cr);
}

function buildConfig(){
  const next = JSON.parse(JSON.stringify(CFG));   // deep clone — preserves identity/api fields
  next.modem = next.modem || {};
  const mh = getVal('modem_host'); if (mh) next.modem.host = mh;
  const mp = parseInt(getVal('modem_port'));     if (mp) next.modem.port = mp;
  next.modem.token = getVal('modem_token') || '';

  next.node = next.node || {};
  next.node.name = getVal('node_name');
  next.node.lat  = parseFloat(getVal('node_lat')) || 0;
  next.node.lon  = parseFloat(getVal('node_lon')) || 0;
  const adm = getVal('node_admin_password'); if (adm) next.node.admin_password = adm;
  const gst = getVal('node_guest_password'); if (gst) next.node.guest_password = gst;

  next.lora = next.lora || {};
  next.lora.preset       = getVal('lora_preset');
  next.lora.freq_mhz     = parseFloat(getVal('lora_freq_mhz'));
  next.lora.bw_khz       = parseFloat(getVal('lora_bw_khz'));
  next.lora.sf           = parseInt(getVal('lora_sf'));
  next.lora.cr           = parseInt(getVal('lora_cr'));
  next.lora.tx_power_dbm = parseInt(getVal('lora_tx_power_dbm'));

  next.repeater = next.repeater || {};
  next.repeater.duty_cycle_pct          = parseInt(getVal('repeater_duty_cycle_pct'));
  next.repeater.advert_interval_min     = parseInt(getVal('repeater_advert_interval_min'));
  next.repeater.flood_advert_interval_h = parseInt(getVal('repeater_flood_advert_interval_h'));
  next.repeater.flood_max_hops          = parseInt(getVal('repeater_flood_max_hops'));
  next.repeater.large_mesh_optim        = !!getVal('repeater_large_mesh_optim');
  next.repeater.loop_detect             = getVal('repeater_loop_detect');
  next.repeater.path_hash_mode          = parseInt(getVal('repeater_path_hash_mode'));
  next.repeater.interference_threshold  = parseInt(getVal('repeater_interference_threshold'));
  next.repeater.agc_reset_interval      = parseInt(getVal('repeater_agc_reset_interval'));
  next.repeater.rx_delay_base           = parseFloat(getVal('repeater_rx_delay_base'));
  next.repeater.tx_delay_factor         = parseFloat(getVal('repeater_tx_delay_factor'));
  next.repeater.direct_tx_delay_factor  = parseFloat(getVal('repeater_direct_tx_delay_factor'));
  next.repeater.multi_acks              = !!getVal('repeater_multi_acks');
  return next;
}

async function save(){
  const msg = $('saveMsg'); msg.textContent='saving…'; msg.className='msg muted';
  let body; try { body = buildConfig(); }
  catch(e){ msg.textContent='build error: '+e.message; msg.className='msg err'; return; }
  const r = await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  const j = await r.json();
  msg.textContent = j.ok ? ('saved · '+(j.applied||'')) : ('error: '+j.error);
  msg.className   = j.ok ? 'msg ok' : 'msg err';
  setTimeout(refreshStatus, 300);
}
async function sendAdvert(){
  const r = await fetch('/api/command',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({command:'advert'})});
  const j = await r.json();
  termLine('cmd', 'advert'); termLine(j.ok ? 'ans' : 'err', j.reply || j.error || '(no reply)');
}
async function restart(){
  if(!confirm('Restart the Meshcore-Linux service? (systemd will bring it back up in ~5 s.)')) return;
  await fetch('/api/reboot',{method:'POST'});
  $('saveMsg').textContent='service exiting — systemd will restart shortly';
  $('saveMsg').className='msg muted';
}
async function refreshStatus(){
  try {
    const r = await fetch('/api/status'); const j = await r.json();
    $('status').textContent = JSON.stringify(j, null, 2);
    const h = j.modem||{}; const u = j.service||{};
    $('meta').textContent = `modem ${h.host}:${h.port} ${h.connected?'·connected':'·DOWN'} ${h.handshake?'·hs ok':''} · uptime ${u.uptime_secs||0}s`;
  } catch(e){ $('status').textContent='(cannot fetch /api/status: '+e+')'; }
}

// ── CLI terminal ───────────────────────────────────────────────────────
const TERM=$('term'); const CLI=$('cli');
let HIST=[]; let HISTI=-1;
function termLine(cls, text){
  const span=document.createElement('span'); span.className=cls; span.textContent=text+'\n';
  TERM.appendChild(span); TERM.scrollTop=TERM.scrollHeight;
}
async function cliSend(){
  const cmd = CLI.value.trim(); if(!cmd) return;
  termLine('cmd', '> '+cmd);
  HIST.push(cmd); HISTI=HIST.length;
  CLI.value='';
  try {
    const r = await fetch('/api/command',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({command:cmd})});
    const j = await r.json();
    termLine(j.ok ? 'ans' : 'err', j.reply || j.error || '(empty)');
  } catch(e){ termLine('err', '(network: '+e+')'); }
}
CLI.addEventListener('keydown', e=>{
  if (e.key==='Enter')  { e.preventDefault(); cliSend(); }
  if (e.key==='ArrowUp'){ e.preventDefault(); if(HISTI>0){HISTI--;CLI.value=HIST[HISTI];} }
  if (e.key==='ArrowDown'){ e.preventDefault();
    if(HISTI<HIST.length-1){HISTI++;CLI.value=HIST[HISTI];}
    else {HISTI=HIST.length; CLI.value='';} }
});

load(); refreshStatus(); setInterval(refreshStatus, 5000);
</script>
</body></html>
)HTML";

// ── helpers ──────────────────────────────────────────────────────────

static uint16_t json_u16(const json& j, const char* path, uint16_t dflt) {
  try {
    auto p = json::json_pointer(path);
    if (!j.contains(p)) return dflt;
    return j.at(p).get<uint16_t>();
  } catch (...) { return dflt; }
}
static std::string json_str(const json& j, const char* path, const std::string& dflt) {
  try {
    auto p = json::json_pointer(path);
    if (!j.contains(p)) return dflt;
    return j.at(p).get<std::string>();
  } catch (...) { return dflt; }
}
static double json_dbl(const json& j, const char* path, double dflt) {
  try {
    auto p = json::json_pointer(path);
    if (!j.contains(p)) return dflt;
    return j.at(p).get<double>();
  } catch (...) { return dflt; }
}
static int json_int(const json& j, const char* path, int dflt) {
  try {
    auto p = json::json_pointer(path);
    if (!j.contains(p)) return dflt;
    return j.at(p).get<int>();
  } catch (...) { return dflt; }
}

// ── ConfigServer ─────────────────────────────────────────────────────

ConfigServer::ConfigServer(LinuxTcpRadio& radio,
                           json& live_config,
                           std::mutex& config_mu,
                           const std::string& config_path)
  : _radio(radio),
    _live_config(live_config),
    _config_mu(config_mu),
    _config_path(config_path)
{}

ConfigServer::~ConfigServer() { stop(); }

bool ConfigServer::start(const std::string& bind_addr, uint16_t port) {
  if (_running.load()) return true;
  _server = std::make_unique<httplib::Server>();
  route(*_server);

  if (!_server->bind_to_port(bind_addr.c_str(), port)) {
    fprintf(stderr, "ConfigServer: bind %s:%u failed\n", bind_addr.c_str(), port);
    _server.reset();
    return false;
  }
  _running.store(true);
  _thread = std::thread([this]() { _server->listen_after_bind(); _running.store(false); });
  return true;
}

void ConfigServer::stop() {
  if (_server) _server->stop();
  if (_thread.joinable()) _thread.join();
  _server.reset();
  _running.store(false);
}

bool ConfigServer::persistConfig(const json& cfg, std::string& err_out) {
  std::ofstream f(_config_path);
  if (!f) { err_out = "cannot open " + _config_path + " for writing"; return false; }
  f << cfg.dump(2) << "\n";
  if (!f) { err_out = "write to " + _config_path + " failed"; return false; }
  return true;
}

void ConfigServer::applyHotReload(const json& prev, const json& next) {
  // Modem endpoint change → forceReconnect via setEndpoint
  std::string prev_host = json_str(prev, "/modem/host", "");
  std::string next_host = json_str(next, "/modem/host", "");
  uint16_t    prev_port = json_u16(prev, "/modem/port", 5055);
  uint16_t    next_port = json_u16(next, "/modem/port", 5055);
  if (next_host != prev_host || next_port != prev_port) {
    if (!next_host.empty()) _radio.setEndpoint(next_host.c_str(), next_port);
  }

  // Auth token (hex string)
  std::string next_token_hex = json_str(next, "/modem/token", "");
  if (next_token_hex != json_str(prev, "/modem/token", "")) {
    uint8_t buf[32]; size_t n = 0;
    for (size_t i = 0; i + 1 < next_token_hex.size() && n < sizeof(buf); i += 2) {
      buf[n++] = (uint8_t)std::strtoul(next_token_hex.substr(i, 2).c_str(), nullptr, 16);
    }
    _radio.setAuthToken(buf, n);
  }

  // LoRa params — push to modem via setLoRaParams (forces fresh SET_CONFIG
  // on next handshake / on next setEndpoint-triggered reconnect).
  _radio.setLoRaParams(
    (float)json_dbl(next, "/lora/freq_mhz",   869.618),
    (float)json_dbl(next, "/lora/bw_khz",     62.5),
    (uint8_t)json_int(next, "/lora/sf",         8),
    (uint8_t)json_int(next, "/lora/cr",         8),
    (int8_t) json_int(next, "/lora/tx_power_dbm", 22),
    (uint16_t)json_int(next, "/lora/syncword", 0x0012),
    (uint8_t)json_int(next, "/lora/preamble_len", 16));

  // Node + repeater settings → push to mesh via CLI bridge (which holds the
  // mesh mutex). We synthesise the same CLI verbs the ESP32 web tool uses,
  // so MeshCore's CommonCLI parses, validates and persists them into
  // NodePrefs. Order matters: name/lat/lon first, then LoRa-affecting
  // verbs, then advert timers (so updateAdvertTimer sees fresh values).
  if (!_cli_bridge) return;
  auto cli = [&](const std::string& v) {
    std::string ignored = _cli_bridge(v);
    (void)ignored;
  };
  auto sNode = [&](const char* path) { return json_str(next, path, ""); };

  std::string nm = sNode("/node/name");
  if (!nm.empty()) cli("set name " + nm);
  cli("set lat "    + std::to_string(json_dbl(next, "/node/lat", 0.0)));
  cli("set lon "    + std::to_string(json_dbl(next, "/node/lon", 0.0)));
  std::string ap = sNode("/node/admin_password");
  if (!ap.empty()) cli("set password " + ap);
  std::string gp = sNode("/node/guest_password");
  if (!gp.empty()) cli("set guest.password " + gp);

  // LoRa params: MeshCore CLI takes all four atomically via `set radio
  // freq,bw,sf,cr`. Individual `set bw/sf/cr` don't exist — only `set freq`
  // is split out. We push both: `set radio` for the atomic update + `set
  // freq` covers the case where only freq changed (older firmware paths).
  {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "set radio %g,%g,%d,%d",
      json_dbl(next, "/lora/freq_mhz",   869.618),
      json_dbl(next, "/lora/bw_khz",     62.5),
      json_int(next, "/lora/sf",         8),
      json_int(next, "/lora/cr",         8));
    cli(buf);
  }
  cli("set freq " + std::to_string(json_dbl(next, "/lora/freq_mhz",  869.618)));
  cli("set tx "   + std::to_string(json_int(next, "/lora/tx_power_dbm", 22)));

  int dc = json_int(next, "/repeater/duty_cycle_pct", 50);
  if (dc >= 1 && dc <= 100) cli("set dutycycle " + std::to_string(dc));

  // CommonCLI uses dotted-key names (not underscores).
  cli("set advert.interval "       + std::to_string(json_int(next, "/repeater/advert_interval_min", 0)));
  cli("set flood.advert.interval " + std::to_string(json_int(next, "/repeater/flood_advert_interval_h", 0)));
  cli("set flood.max "             + std::to_string(json_int(next, "/repeater/flood_max_hops", 64)));
  cli("set int.thresh "            + std::to_string(json_int(next, "/repeater/interference_threshold", 0)));
  cli("set agc.reset.interval "    + std::to_string(json_int(next, "/repeater/agc_reset_interval", 0)));
  cli("set rxdelay "               + std::to_string(json_dbl(next, "/repeater/rx_delay_base", 0.0)));
  cli("set txdelay "               + std::to_string(json_dbl(next, "/repeater/tx_delay_factor", 0.5)));
  cli("set direct.txdelay "        + std::to_string(json_dbl(next, "/repeater/direct_tx_delay_factor", 0.3)));
  cli(std::string("set multi.acks ") + (json_int(next, "/repeater/multi_acks", 0) ? "1" : "0"));

  std::string ld = sNode("/repeater/loop_detect");
  if (!ld.empty()) cli("set loop.detect " + ld);
  cli("set path.hash.mode " + std::to_string(json_int(next, "/repeater/path_hash_mode", 1)));

  // Large mesh optimisation — when enabled, suppress periodic adverts.
  if (json_int(next, "/repeater/large_mesh_optim", 0)) cli("set advert.interval 0");
}

void ConfigServer::route(httplib::Server& s) {
  s.Get("/", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(INDEX_HTML, "text/html; charset=utf-8");
  });

  s.Get("/api/config", [this](const httplib::Request&, httplib::Response& res) {
    // Start from on-disk config (modem endpoint, identity, anything not in
    // _prefs). Then let the live-overlay callback (wired in main.cpp) refresh
    // node/lora/repeater scalars from current NodePrefs — so values changed
    // via CLI bridge OR over LoRa from the mobile app show up here.
    nlohmann::json out;
    {
      std::lock_guard<std::mutex> lk(_config_mu);
      out = _live_config;
    }
    if (_live_overlay) _live_overlay(out);
    res.set_content(out.dump(2), "application/json");
  });

  s.Post("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
    json next;
    try { next = json::parse(req.body); }
    catch (const std::exception& e) {
      res.status = 400;
      res.set_content(json{{"ok",false},{"error", std::string("parse: ")+e.what()}}.dump(),
                      "application/json");
      return;
    }
    json prev;
    {
      std::lock_guard<std::mutex> lk(_config_mu);
      prev = _live_config;
    }
    std::string err;
    if (!persistConfig(next, err)) {
      res.status = 500;
      res.set_content(json{{"ok",false},{"error",err}}.dump(), "application/json");
      return;
    }
    {
      std::lock_guard<std::mutex> lk(_config_mu);
      _live_config = next;
    }
    applyHotReload(prev, next);
    res.set_content(json{
      {"ok", true},
      {"applied", "modem endpoint + LoRa params hot-applied; api_port change needs restart"}
    }.dump(), "application/json");
  });

  s.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
    uint64_t now = (uint64_t)time(nullptr);
    uint64_t up0 = _uptime_start_secs.load();
    json j = {
      {"modem", {
        {"host",        _radio.getModemHost()},
        {"port",        _radio.getModemPort()},
        {"connected",   _radio.isConnected()},
        {"handshake",   _radio.isHandshakeComplete()},
        {"rx_mode",     _radio.isInRecvMode()},
        {"reconnects",  _radio.getReconnectCount()},
      }},
      {"stats", {
        {"rx",       _radio.getRxCount()},
        {"tx",       _radio.getTxCount()},
        {"pong",     _radio.getPongCount()},
        {"crc_err",  _radio.getCrcErrors()},
        {"noise_dbm", _radio.getNoiseFloor()},
        {"last_rssi", _radio.getLastRSSI()},
        {"last_snr",  _radio.getLastSNR()},
      }},
      {"service", {
        {"uptime_secs", (up0 > 0 && now >= up0) ? (now - up0) : 0},
      }},
    };
    res.set_content(j.dump(2), "application/json");
  });

  s.Post("/api/command", [this](const httplib::Request& req, httplib::Response& res) {
    if (!_cli_bridge) {
      res.status = 503;
      res.set_content(json{{"ok",false},{"error","no CLI bridge wired"}}.dump(),
                      "application/json");
      return;
    }
    std::string cmd;
    try {
      auto body = json::parse(req.body);
      cmd = body.value("command", "");
    } catch (...) {
      // also accept plain text bodies for curl simplicity
      cmd = req.body;
    }
    if (cmd.empty()) {
      res.status = 400;
      res.set_content(json{{"ok",false},{"error","empty command"}}.dump(),
                      "application/json");
      return;
    }
    std::string reply = _cli_bridge(cmd);
    res.set_content(json{{"ok",true},{"command",cmd},{"reply",reply}}.dump(2),
                    "application/json");
  });

  s.Post("/api/reboot", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(json{{"ok",true},{"msg","exiting — systemd will restart"}}.dump(),
                    "application/json");
    // Schedule exit shortly so the response can flush.
    std::thread([]{ std::this_thread::sleep_for(std::chrono::milliseconds(150)); std::_Exit(0); }).detach();
  });
}
