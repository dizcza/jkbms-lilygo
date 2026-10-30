// Loads /api/latest once and renders one card per BMS; reload the page for new data.

const charts = {};

function esc(text) {
  const div = document.createElement("div");
  div.textContent = text ?? "";
  return div.innerHTML;
}

// "1d 2h 3m 4s ago", leading zero parts omitted
function ago(seconds) {
  let s = Math.max(0, Math.round(seconds));
  const parts = [];
  for (const [unit, size] of [["d", 86400], ["h", 3600], ["m", 60]]) {
    if (s >= size || parts.length) parts.push(`${Math.floor(s / size)}${unit}`);
    s %= size;
  }
  parts.push(`${s}s`);
  return `${parts.join(" ")} ago`;
}

function agoSpan(ts, now) {
  return `<span>${ago(now - ts)}</span>`;
}

function dateTime(ts) {
  return new Date(ts * 1000).toLocaleString();
}

function fmt(value, digits, unit) {
  return value === undefined || value === null ? "–" : `${Number(value).toFixed(digits)}${unit}`;
}

function onOff(on) {
  return on ? '<span class="ok">ON</span>' : '<span class="dim">OFF</span>';
}

function renderCells(bms) {
  const cells = bms.cells || [];
  const max = Math.max(...cells);
  const min = Math.min(...cells);
  return cells.map((v, i) => {
    const classes = ["cell"];
    if (max > min && v === max) classes.push("max");
    if (max > min && v === min) classes.push("min");
    if (bms.wire_warning & (1 << i)) classes.push("wire");
    const r = bms.resistances ? `${Math.round(bms.resistances[i] * 1000)} mΩ` : "";
    return `<div class="${classes.join(" ")}"><div class="n">Cell ${i + 1}</div>` +
           `<div class="v">${v.toFixed(3)} V</div><div class="r">${r}</div></div>`;
  }).join("");
}

function onOffText(value) {
  return value === undefined ? "–" : (value ? "ON" : "OFF");
}

function renderSettings(settings) {
  if (!settings) return '<p class="dim">Not received yet.</p>';
  const rows = [
    ["Charge switch", settings.charge_enabled ? "ON" : "OFF"],
    ["Discharge switch", settings.discharge_enabled ? "ON" : "OFF"],
    ["Balancer switch", settings.balancer_enabled ? "ON" : "OFF"],
    ["Cells", settings.cell_count],
    ["Capacity", fmt(settings.capacity, 0, " Ah")],
    ["Cell UVP / recovery", `${fmt(settings.cell_uvp, 3, "")} / ${fmt(settings.cell_uvp_recovery, 3, " V")}`],
    ["SOC 0% voltage", fmt(settings.cell_soc0, 3, " V")],
    ["Power off voltage", fmt(settings.power_off, 3, " V")],
    ["Cell OVP / recovery", `${fmt(settings.cell_ovp, 3, "")} / ${fmt(settings.cell_ovp_recovery, 3, " V")}`],
    ["SOC 100% voltage", fmt(settings.cell_soc100, 3, " V")],
    ["Smart sleep", `${settings.smart_sleep_enabled ? "ON" : "OFF"}, ${fmt(settings.smart_sleep, 3, " V")}, ` +
                    `${settings.smart_sleep_delay_h} h`],
    ["Balance start / trigger", `${fmt(settings.balance_start, 3, " V")} / ` +
                                `${fmt(settings.balance_trigger * 1000, 0, " mV")}`],
    ["Max balance current", fmt(settings.max_balance_current, 1, " A")],
    ["Max charge / discharge", `${fmt(settings.max_charge_current, 0, "")} / ` +
                               `${fmt(settings.max_discharge_current, 0, " A")}`],
    ["Charge OCP delay / recovery", `${settings.charge_ocp_delay ?? "–"} / ${settings.charge_ocp_recovery ?? "–"} s`],
    ["Discharge OCP delay / recovery",
     `${settings.discharge_ocp_delay ?? "–"} / ${settings.discharge_ocp_recovery ?? "–"} s`],
    ["Short circuit delay / recovery", `${settings.scp_delay_us ?? "–"} µs / ${settings.scp_recovery ?? "–"} s`],
    ["Request charge / float voltage", `${fmt(settings.cell_rcv, 3, "")} / ${fmt(settings.cell_rfv, 3, " V")}`],
    ["Charge OTP / recovery", `${settings.charge_otp} / ${settings.charge_otp_recovery ?? "–"} °C`],
    ["Charge UTP / recovery", `${settings.charge_utp} / ${settings.charge_utp_recovery ?? "–"} °C`],
    ["Discharge OTP / recovery", `${settings.discharge_otp} / ${settings.discharge_otp_recovery ?? "–"} °C`],
    ["Discharge UTP / recovery", `${settings.discharge_utp} / ${settings.discharge_utp_recovery ?? "–"} °C`],
    ["MOSFET OTP / recovery", `${settings.mosfet_otp} / ${settings.mosfet_otp_recovery ?? "–"} °C`],
    ["Heating", `${settings.heating_enabled ? "ON" : "OFF"}` +
                (settings.heating_start !== undefined ? `, ${settings.heating_start}…${settings.heating_stop} °C` : "")],
    ["Timed stored data", onOffText(settings.timed_stored_data)],
    ["Charging float mode", onOffText(settings.charging_float_mode)],
    ["Special charger", onOffText(settings.special_charger)],
    ["Display always on", onOffText(settings.display_always_on)],
    ["Temperature sensors disabled", onOffText(settings.disable_temp_sensors)],
    ["Disable PCL module", onOffText(settings.disable_pcl_module)],
    ["GPS heartbeat", onOffText(settings.gps_heartbeat)],
    ["Port", settings.port_rs485 === undefined ? "–" : (settings.port_rs485 ? "RS485" : "CAN")],
    ["Precharge time", settings.precharge === undefined ? "–" : `${settings.precharge} s`],
    ["Device address", settings.device_address ?? "–"],
    ["Wire resistance per cell", (settings.wire_resistance_mohm || []).join(", ") +
                                 (settings.wire_resistance_mohm ? " mΩ" : "–")],
  ];
  return "<table>" + rows.map(([k, v]) => `<tr><th>${k}</th><td>${esc(String(v))}</td></tr>`).join("") +
         "</table>";
}

function renderEvents(events) {
  if (!events || !events.length) return '<p class="dim">No events received yet.</p>';
  return "<table><tr><th>#</th><th>Time</th><th>Event</th></tr>" + events.map(e =>
    `<tr><td>${e.number}</td><td>${dateTime(e.ts)}</td><td>${esc(e.label)}</td></tr>`).join("") + "</table>";
}

function details(id, title, body) {
  return `<details id="${id}"><summary>${title}</summary>${body}</details>`;
}

// boards with the remote-maintenance firmware report their diagnostics
function renderBoard(d, now) {
  const fw = d.fw || {};
  const diag = d.diag || {};
  const lines = [`last report ${agoSpan(d.received, now)} · upload #${d.wake ?? "?"}`];
  if (fw.version) {
    lines.push(`firmware ${esc(fw.version)} built ${esc(fw.built)} (${esc((fw.elf || "").slice(0, 8))}) · ` +
               `uptime ${ago(diag.uptime).replace(" ago", "")} · reset: ${esc(diag.reset)} · ` +
               `Wi-Fi ${diag.wifi_rssi} dBm · heap ${Math.round(diag.heap / 1024)} KB` +
               (fw.ota_error ? ` · <span class="error">OTA: ${esc(fw.ota_error)}</span>` : ""));
    for (const slot of diag.slots || []) {
      lines.push(`slot ${slot.slot}: ${esc(slot.link)}${slot.mac ? " · " + slot.mac : ""}`);
    }
  } else {
    lines.push("old firmware without diagnostics");
  }
  const ble = (diag.ble || []).map(b =>
    `<tr><td>${b.mac}</td><td>${esc(b.name)}</td><td>${b.rssi} dBm</td><td>${b.jk ? "JK" : ""}</td>` +
    `<td>${b.age} s ago</td></tr>`).join("");
  return `
  <section class="card">
    <h2>Board ${esc(d.device)}</h2>
    ${lines.map(l => `<div class="dim">${l}</div>`).join("")}
    ${ble ? details(`ble-${d.device}`, `Bluetooth devices around (${diag.ble.length})`,
                    `<table><tr><th>MAC</th><th>Name</th><th>RSSI</th><th></th><th>Seen</th></tr>${ble}</table>`)
          : ""}
  </section>`;
}

// MACs the boards with diagnostics watch now
function watchedMacs(devices) {
  const macs = new Set();
  let known = false;
  for (const d of devices) {
    if (!d.diag) continue;
    known = true;
    (d.diag.slots || []).forEach(s => s.mac && macs.add(s.mac));
  }
  return known ? macs : null;
}

function renderCard(bms, now, watched) {
  const fresh = bms.online && !bms.stale;
  const dotColor = fresh ? "var(--ok)" : "var(--error)";
  const errors = bms.error_labels || [];
  // when online, the data is as old as the last upload of the board
  const status = fresh ? "" :
    `offline${bms.error ? " (" + esc(bms.error) + ")" : ""} · last data ` +
    `${bms.updated ? agoSpan(bms.updated, now) : "never"} · `;
  const id = bms.mac.replace(/:/g, "");
  const current = bms.current ?? 0;
  const currentClass = current > 0.05 ? "ok" : current < -0.05 ? "warn" : "";
  return `
  <section class="card${errors.length ? " alarm" : ""}">
    <h2><span class="dot" style="background:${dotColor}"></span>${esc(bms.name || bms.mac)}</h2>
    <div class="dim">${esc(bms.model || "")} · ${bms.mac} · hardware version ${esc(bms.hw || "?")} ·
      software version ${esc(bms.sw || "?")}</div>
    <div class="dim">${status}board report ${agoSpan(bms.received, now)}</div>
    ${watched && !watched.has(bms.mac) ? '<p class="warn">No board watches this BMS any more.</p>' : ""}
    ${errors.length ? `<p class="error">Errors: ${errors.map(esc).join(", ")}</p>` : ""}
    <div class="stats">
      <div class="stat"><div class="label">Pack</div><div class="value">${fmt(bms.voltage, 2, " V")}</div></div>
      <div class="stat"><div class="label">Current</div>
        <div class="value ${currentClass}">${fmt(bms.current, 2, " A")}</div></div>
      <div class="stat"><div class="label">Power</div><div class="value">${fmt(bms.power, 0, " W")}</div></div>
      <div class="stat"><div class="label">SOC</div><div class="value">${fmt(bms.soc, 0, " %")}</div>
        <div class="soc-bar"><div style="width:${bms.soc ?? 0}%"></div></div></div>
      <div class="stat"><div class="label">Diff</div>
        <div class="value">${fmt(bms.diff === undefined ? undefined : bms.diff * 1000, 0, " mV")}</div></div>
      <div class="stat"><div class="label">CHG / DSG / BAL</div>
        <div class="value">${onOff(bms.charging)} ${onOff(bms.discharging)} ${onOff(bms.balancing)}</div></div>
      <div class="stat"><div class="label">T1 / T2 / MOS</div>
        <div class="value">${fmt(bms.t1, 0, "")} / ${fmt(bms.t2, 0, "")} / ${fmt(bms.t_mos, 0, " °C")}</div></div>
      <div class="stat"><div class="label">Capacity</div>
        <div class="value">${fmt(bms.capacity_remaining, 1, "")} / ${fmt(bms.capacity_nominal, 0, " Ah")}</div></div>
      <div class="stat"><div class="label">Cycles</div><div class="value">${fmt(bms.cycles, 0, "")}</div></div>
    </div>
    <div class="cells">${renderCells(bms)}</div>
    <div class="chart-wrap"><canvas id="chart-${id}"></canvas></div>
    ${details(`settings-${id}`, "Settings", renderSettings(bms.settings))}
    ${details(`events-${id}`, `Logbook (${(bms.events || []).length} events)`, renderEvents(bms.events))}
  </section>`;
}

async function drawChart(bms) {
  const id = bms.mac.replace(/:/g, "");
  const resp = await fetch(`/api/history?mac=${encodeURIComponent(bms.mac)}&hours=48`);
  if (!resp.ok) return;
  const rows = await resp.json();
  const canvas = document.getElementById(`chart-${id}`);
  if (!canvas) return;
  // x is the time in ms on a linear scale, so that gaps between readings show as gaps
  const timeLabel = ms => new Date(ms).toLocaleString([], {
    month: "short", day: "numeric", hour: "2-digit", minute: "2-digit"});
  const points = key => rows.map(r => ({x: r.ts * 1000, y: r[key]}));
  charts[id] = new Chart(canvas, {
    type: "line",
    data: {
      datasets: [
        {label: "Max cell, V", data: points("max_cell"), borderColor: "#ff5252", yAxisID: "cell"},
        {label: "Min cell, V", data: points("min_cell"), borderColor: "#42a5f5", yAxisID: "cell"},
        {label: "SOC, %", data: points("soc"), borderColor: "#3ddc84", yAxisID: "pct"},
        {label: "Current, A", data: points("current"), borderColor: "#ffa726", yAxisID: "amp"},
      ],
    },
    options: {
      animation: false,
      maintainAspectRatio: false,
      elements: {point: {radius: 0}, line: {borderWidth: 1.5}},
      interaction: {mode: "index", intersect: false},
      plugins: {
        legend: {labels: {color: "#e6edf3"}},
        tooltip: {callbacks: {title: items => timeLabel(items[0].parsed.x)}},
      },
      scales: {
        x: {type: "linear", ticks: {color: "#8a949e", maxTicksLimit: 8, callback: timeLabel},
            grid: {color: "#2a313a"}},
        cell: {position: "left", ticks: {color: "#8a949e"}, grid: {color: "#2a313a"}},
        pct: {position: "right", min: 0, max: 100, ticks: {color: "#3ddc84"}, grid: {display: false}},
        amp: {position: "right", ticks: {color: "#ffa726"}, grid: {display: false}},
      },
    },
  });
}

async function refresh() {
  const resp = await fetch("/api/latest");
  if (resp.status === 401) {
    location.href = "/login";
    return;
  }
  render(await resp.json());
}

function render(data) {
  const list = document.getElementById("bms-list");
  if (!data.bms.length && !data.devices.length) {
    list.innerHTML = '<p class="dim">No data from the board yet.</p>';
    return;
  }
  Object.values(charts).forEach(c => c.destroy());
  Object.keys(charts).forEach(k => delete charts[k]);
  const watched = watchedMacs(data.devices);
  list.innerHTML = data.bms.map(b => renderCard(b, data.now, watched)).join("") +
                   data.devices.map(d => renderBoard(d, data.now)).join("");
  data.bms.forEach(drawChart);
  document.getElementById("updated").textContent = `Loaded ${new Date().toLocaleTimeString()}`;
}


refresh();
