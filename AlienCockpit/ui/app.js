// AlienCockpit UI Controller
const hasWebView = !!(window.chrome && window.chrome.webview);

const state = {
  devices: [],
  profiles: [],
  activeProfileId: 0,
  settings: { lightsOn: true, brightness: 255, startWindows: false, startMinimized: true },
  selectedTargets: new Set(), // key IDs or zone IDs
  selectedHwLights: new Set(), // direct physical LED IDs from Hardware LED Mapper
  selectedDevicePid: 0,
  activeView: 'keyboard',
  effect: 'static',
  colorPrimary: '#00f0ff',
  colorSecondary: '#7b2cbf',
  speed: 50,
  keyColors: {},
  zoneColors: {},
  fanCurve: [
    { temp: 35, boost: 20 },
    { temp: 50, boost: 40 },
    { temp: 65, boost: 65 },
    { temp: 80, boost: 88 },
    { temp: 92, boost: 100 }
  ],
  isDraggingSelect: false
};

function sendCommand(payload) {
  if (hasWebView) {
    window.chrome.webview.postMessage(payload);
  } else {
    // Local browser preview fallback
    console.log('[Preview Mode] Command:', payload);
  }
}

if (hasWebView) {
  window.chrome.webview.addEventListener('message', (event) => {
    const data = event.data;
    if (!data) return;
    if (data.type === 'state') {
      applyFullState(data);
    } else if (data.type === 'telemetry') {
      applyTelemetry(data);
    }
  });
}

// ANSI Full/Laptop Keyboard Layout
const KEYBOARD_ROWS = [
  [
    { id: 1, label: 'ESC', group: 'mods' }, { id: 59, label: 'F1', group: 'fn' }, { id: 60, label: 'F2', group: 'fn' },
    { id: 61, label: 'F3', group: 'fn' }, { id: 62, label: 'F4', group: 'fn' }, { id: 63, label: 'F5', group: 'fn' },
    { id: 64, label: 'F6', group: 'fn' }, { id: 65, label: 'F7', group: 'fn' }, { id: 66, label: 'F8', group: 'fn' },
    { id: 67, label: 'F9', group: 'fn' }, { id: 68, label: 'F10', group: 'fn' }, { id: 87, label: 'F11', group: 'fn' },
    { id: 88, label: 'F12', group: 'fn' }, { id: 83, label: 'DEL', group: 'mods' }
  ],
  [
    { id: 41, label: '`' }, { id: 2, label: '1' }, { id: 3, label: '2' }, { id: 4, label: '3' },
    { id: 5, label: '4' }, { id: 6, label: '5' }, { id: 7, label: '6' }, { id: 8, label: '7' },
    { id: 9, label: '8' }, { id: 10, label: '9' }, { id: 11, label: '0' }, { id: 12, label: '-' },
    { id: 13, label: '=' }, { id: 14, label: 'BKSP', cls: 'w-2', group: 'mods' }
  ],
  [
    { id: 15, label: 'TAB', cls: 'w-15', group: 'mods' }, { id: 16, label: 'Q' }, { id: 17, label: 'W', group: 'wasd' },
    { id: 18, label: 'E' }, { id: 19, label: 'R' }, { id: 20, label: 'T' }, { id: 21, label: 'Y' },
    { id: 22, label: 'U' }, { id: 23, label: 'I' }, { id: 24, label: 'O' }, { id: 25, label: 'P' },
    { id: 26, label: '[' }, { id: 27, label: ']' }, { id: 43, label: '\\', cls: 'w-15' }
  ],
  [
    { id: 58, label: 'CAPS', cls: 'w-175', group: 'mods' }, { id: 30, label: 'A', group: 'wasd' },
    { id: 31, label: 'S', group: 'wasd' }, { id: 32, label: 'D', group: 'wasd' }, { id: 33, label: 'F' },
    { id: 34, label: 'G' }, { id: 35, label: 'H' }, { id: 36, label: 'J' }, { id: 37, label: 'K' },
    { id: 38, label: 'L' }, { id: 39, label: ';' }, { id: 40, label: "'" },
    { id: 28, label: 'ENTER', cls: 'w-225', group: 'mods' }
  ],
  [
    { id: 42, label: 'SHIFT', cls: 'w-225', group: 'mods' }, { id: 44, label: 'Z' }, { id: 45, label: 'X' },
    { id: 46, label: 'C' }, { id: 47, label: 'V' }, { id: 48, label: 'B' }, { id: 49, label: 'N' },
    { id: 50, label: 'M' }, { id: 51, label: ',' }, { id: 52, label: '.' }, { id: 53, label: '/' },
    { id: 54, label: 'RSHIFT', cls: 'w-175', group: 'mods' }, { id: 72, label: '▲', group: 'arrows' }
  ],
  [
    { id: 29, label: 'CTRL', cls: 'w-15', group: 'mods' }, { id: 91, label: 'WIN', group: 'mods' },
    { id: 56, label: 'ALT', cls: 'w-15', group: 'mods' }, { id: 57, label: 'SPACE', cls: 'w-6' },
    { id: 100, label: 'RALT', group: 'mods' }, { id: 101, label: 'FN', group: 'mods' },
    { id: 75, label: '◀', group: 'arrows' }, { id: 80, label: '▼', group: 'arrows' }, { id: 77, label: '▶', group: 'arrows' }
  ]
];

function buildKeyboard() {
  const container = document.getElementById('kbd-grid');
  container.innerHTML = '';

  KEYBOARD_ROWS.forEach((row) => {
    const rowEl = document.createElement('div');
    rowEl.className = 'kbd-row';

    row.forEach((key) => {
      const btn = document.createElement('div');
      btn.className = `keycap ${key.cls || ''}`;
      btn.textContent = key.label;
      btn.dataset.id = key.id;
      btn.dataset.name = key.label;
      if (key.group) btn.dataset.group = key.group;

      const color = state.keyColors[key.id] || '#00f0ff';
      btn.style.boxShadow = `inset 0 -3px 0 ${color}, 0 0 8px ${color}33`;

      btn.addEventListener('mousedown', (e) => {
        state.isDraggingSelect = true;
        if (!e.ctrlKey && !e.shiftKey) state.selectedTargets.clear();
        toggleTargetSelection(key.id);
      });

      btn.addEventListener('mouseenter', () => {
        if (state.isDraggingSelect) {
          state.selectedTargets.add(String(key.id));
          refreshSelectionUI();
        }
      });

      rowEl.appendChild(btn);
    });

    container.appendChild(rowEl);
  });

  window.addEventListener('mouseup', () => {
    state.isDraggingSelect = false;
  });
}

function toggleTargetSelection(id) {
  const key = String(id);
  if (state.selectedTargets.has(key)) state.selectedTargets.delete(key);
  else state.selectedTargets.add(key);
  refreshSelectionUI();
}

function refreshSelectionUI() {
  document.querySelectorAll('.keycap').forEach((el) => {
    el.classList.toggle('selected', state.selectedTargets.has(el.dataset.id));
  });
  document.querySelectorAll('.mouse-zone, .chassis-zone').forEach((el) => {
    el.classList.toggle('selected', state.selectedTargets.has(el.dataset.zoneId));
  });

  const count = state.selectedTargets.size;
  document.getElementById('selected-count').textContent = `${count} selected`;
  const preview = document.getElementById('selected-names');
  if (count === 0) {
    preview.textContent = 'Click keys or zones on the left';
  } else {
    preview.textContent = `Targets: ${Array.from(state.selectedTargets).slice(0, 8).join(', ')}${count > 8 ? '…' : ''}`;
  }
}

function initNavigation() {
  document.querySelectorAll('.nav-item').forEach((btn) => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.nav-item').forEach((b) => b.classList.remove('active'));
      document.querySelectorAll('.tab-panel').forEach((p) => p.classList.remove('active'));
      btn.classList.add('active');
      document.getElementById(`tab-${btn.dataset.tab}`).classList.add('active');
    });
  });

  // Device view switcher (Keyboard / Mouse / Chassis)
  document.querySelectorAll('.device-switcher .seg-btn').forEach((btn) => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.device-switcher .seg-btn').forEach((b) => b.classList.remove('active'));
      document.querySelectorAll('.device-canvas').forEach((c) => c.classList.remove('active'));
      btn.classList.add('active');
      state.activeView = btn.dataset.view;
      document.getElementById(`view-${state.activeView}`).classList.add('active');
      document.getElementById('kbd-quick-bar').style.display = state.activeView === 'keyboard' ? 'flex' : 'none';
      state.selectedTargets.clear();
      refreshSelectionUI();
    });
  });

  // Quick Select buttons
  document.querySelectorAll('.qs-btn[data-select]').forEach((btn) => {
    btn.addEventListener('click', () => {
      const mode = btn.dataset.select;
      state.selectedHwLights.clear();
      state.selectedTargets.clear();
      if (mode !== 'clear') {
        document.querySelectorAll('.keycap').forEach((k) => {
          if (mode === 'all' || k.dataset.group === mode) {
            state.selectedTargets.add(k.dataset.id);
          }
        });
      }
      refreshSelectionUI();
    });
  });

  // Mouse & Chassis zones
  document.querySelectorAll('.mouse-zone, .chassis-zone').forEach((zone) => {
    zone.addEventListener('click', (e) => {
      state.selectedHwLights.clear();
      if (!e.ctrlKey && !e.shiftKey) state.selectedTargets.clear();
      toggleTargetSelection(zone.dataset.zoneId);
      const labelId = state.activeView === 'mouse' ? 'mouse-zone-label' : 'chassis-zone-label';
      document.getElementById(labelId).textContent = `Selected: ${zone.dataset.zoneName}`;
    });
  });
}

function initLightingControls() {
  const cPrimary = document.getElementById('color-primary');
  const hPrimary = document.getElementById('hex-primary');
  const cSecondary = document.getElementById('color-secondary');

  cPrimary.addEventListener('input', () => {
    state.colorPrimary = cPrimary.value;
    hPrimary.value = cPrimary.value.toUpperCase();
  });

  hPrimary.addEventListener('change', () => {
    if (/^#[0-9a-fA-F]{6}$/.test(hPrimary.value)) {
      state.colorPrimary = hPrimary.value;
      cPrimary.value = hPrimary.value;
    }
  });

  cSecondary.addEventListener('input', () => {
    state.colorSecondary = cSecondary.value;
  });

  document.querySelectorAll('.swatch').forEach((sw) => {
    sw.addEventListener('click', () => {
      state.colorPrimary = sw.dataset.color;
      cPrimary.value = sw.dataset.color;
      hPrimary.value = sw.dataset.color.toUpperCase();
    });
  });

  document.querySelectorAll('.eff-btn').forEach((btn) => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.eff-btn').forEach((b) => b.classList.remove('active'));
      btn.classList.add('active');
      state.effect = btn.dataset.eff;
      const showSec = ['morph', 'breathing', 'pulse'].includes(state.effect);
      document.getElementById('secondary-color-wrap').style.display = showSec ? 'flex' : 'none';
    });
  });

  document.getElementById('effect-speed').addEventListener('input', (e) => {
    state.speed = parseInt(e.target.value, 10);
  });

  document.getElementById('btn-apply-lighting').addEventListener('click', () => {
    const uiTargets = Array.from(state.selectedTargets).map((x) => parseInt(x, 10));
    const hwTargets = Array.from(state.selectedHwLights);
    if (uiTargets.length === 0 && hwTargets.length === 0) return;

    // Update UI visual preview immediately
    uiTargets.forEach((id) => {
      state.keyColors[id] = state.colorPrimary;
      const keyEl = document.querySelector(`.keycap[data-id="${id}"]`);
      if (keyEl) {
        keyEl.style.boxShadow = `inset 0 -3px 0 ${state.colorPrimary}, 0 0 12px ${state.colorPrimary}55`;
      }
      const zoneEl = document.querySelector(`.mouse-zone[data-zone-id="${id}"], .chassis-zone[data-zone-id="${id}"]`);
      if (zoneEl) {
        if (zoneEl.tagName.toLowerCase() === 'path') zoneEl.style.stroke = state.colorPrimary;
        else zoneEl.style.fill = state.colorPrimary;
      }
    });

    const dev = state.devices.find((d) => d.pid === state.selectedDevicePid) || state.devices[0];
    let resolvedIds = hwTargets;

    if (resolvedIds.length === 0) {
      if (dev && dev.lights && dev.lights.length > 0) {
        const nonIndicatorLights = dev.lights.filter((l) => !(l.flags & 3));
        const usableLights = nonIndicatorLights.length > 0 ? nonIndicatorLights : dev.lights;

        if (state.activeView === 'keyboard') {
          const matched = [];
          uiTargets.forEach((sc) => {
            const found = dev.lights.find((l) => l.scancode === sc);
            if (found) matched.push(found.id);
          });
          if (matched.length > 0) {
            resolvedIds = Array.from(new Set(matched));
          } else {
            // Zone-based keyboard without per-key scancodes: apply to usable hardware zones
            resolvedIds = usableLights.map((l) => l.id);
          }
        } else {
          const matched = [];
          uiTargets.forEach((zid) => {
            const exact = dev.lights.find((l) => l.id === zid);
            if (exact) matched.push(exact.id);
            else if (usableLights.length > 0) matched.push(usableLights[zid % usableLights.length].id);
          });
          resolvedIds = Array.from(new Set(matched));
        }
      } else {
        resolvedIds = uiTargets;
      }
    }

    sendCommand({
      cmd: 'applyZoneColor',
      pid: dev ? dev.pid : state.selectedDevicePid,
      lightIds: resolvedIds,
      color: state.colorPrimary,
      color2: state.colorSecondary,
      effect: state.effect,
      speed: state.speed,
      zoneName: `${state.activeView.toUpperCase()} Zone`
    });
  });

  // Master controls
  const brightSlider = document.getElementById('master-brightness');
  brightSlider.addEventListener('input', () => {
    const pct = Math.round((brightSlider.value / 255) * 100);
    document.getElementById('master-brightness-val').textContent = `${pct}%`;
    sendCommand({ cmd: 'setBrightness', value: parseInt(brightSlider.value, 10) });
  });

  const btnLights = document.getElementById('btn-master-lights');
  btnLights.addEventListener('click', () => {
    state.settings.lightsOn = !state.settings.lightsOn;
    updateLightsButton();
    sendCommand({ cmd: 'toggleLights', on: state.settings.lightsOn });
  });
}

function updateLightsButton() {
  const btn = document.getElementById('btn-master-lights');
  btn.classList.toggle('active', state.settings.lightsOn);
  btn.innerHTML = `<span class="dot"></span> ${state.settings.lightsOn ? 'LIGHTS ON' : 'LIGHTS OFF'}`;
}

function renderFanCurve() {
  const svg = document.getElementById('fan-curve-svg');
  const w = 640, h = 240, pad = 34;
  svg.innerHTML = '';

  // Grid lines
  for (let i = 0; i <= 4; i++) {
    const y = pad + ((h - 2 * pad) * i) / 4;
    const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
    line.setAttribute('x1', pad); line.setAttribute('x2', w - pad);
    line.setAttribute('y1', y); line.setAttribute('y2', y);
    line.setAttribute('stroke', 'rgba(255,255,255,0.07)');
    svg.appendChild(line);
  }

  const toX = (t) => pad + ((t - 20) / 80) * (w - 2 * pad);
  const toY = (b) => h - pad - (b / 100) * (h - 2 * pad);

  const ptsStr = state.fanCurve.map((p) => `${toX(p.temp)},${toY(p.boost)}`).join(' ');
  const poly = document.createElementNS('http://www.w3.org/2000/svg', 'polyline');
  poly.setAttribute('points', ptsStr);
  poly.setAttribute('fill', 'none');
  poly.setAttribute('stroke', '#00f0ff');
  poly.setAttribute('stroke-width', '3');
  svg.appendChild(poly);

  state.fanCurve.forEach((p, idx) => {
    const cx = toX(p.temp), cy = toY(p.boost);
    const c = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
    c.setAttribute('cx', cx); c.setAttribute('cy', cy); c.setAttribute('r', '7');
    c.setAttribute('fill', '#05070d'); c.setAttribute('stroke', '#00f0ff'); c.setAttribute('stroke-width', '3');
    c.style.cursor = 'grab';

    c.addEventListener('mousedown', () => {
      const onMove = (ev) => {
        const rect = svg.getBoundingClientRect();
        const relX = ((ev.clientX - rect.left) / rect.width) * w;
        const relY = ((ev.clientY - rect.top) / rect.height) * h;
        const temp = Math.max(20, Math.min(100, Math.round(20 + ((relX - pad) / (w - 2 * pad)) * 80)));
        const boost = Math.max(0, Math.min(100, Math.round((1 - (relY - pad) / (h - 2 * pad)) * 100)));
        state.fanCurve[idx] = { temp, boost };
        renderFanCurve();
      };
      const onUp = () => {
        window.removeEventListener('mousemove', onMove);
        window.removeEventListener('mouseup', onUp);
      };
      window.addEventListener('mousemove', onMove);
      window.addEventListener('mouseup', onUp);
    });

    svg.appendChild(c);
  });
}

function applyTelemetry(data) {
  if (data.system) {
    document.getElementById('hud-cpu').textContent = `${data.system.cpu || 0}%`;
    document.getElementById('hud-temp').textContent = `${data.system.temp || '--'}°C`;
    document.getElementById('hud-batt').textContent = `${data.system.batt || '--'}%`;
  }

  if (data.batteries) {
    const grid = document.getElementById('battery-grid');
    grid.innerHTML = '';
    if (data.batteries.length === 0) {
      grid.innerHTML = '<div class="card">No battery-powered devices detected.</div>';
    } else {
      data.batteries.forEach((b) => {
        const card = document.createElement('div');
        card.className = 'batt-card';
        card.innerHTML = `
          <div class="batt-top">
            <div>
              <div class="batt-type">${b.type}</div>
              <strong>${b.name}</strong>
            </div>
            <div class="batt-pct accent">${b.percentage}%</div>
          </div>
          <div class="batt-bar"><div class="batt-fill" style="width:${b.percentage}%"></div></div>
          <div class="hint-text">${b.charging ? '⚡ Charging' : (b.acOnline ? 'Plugged In' : 'On Battery')}</div>
        `;
        grid.appendChild(card);
      });
    }
  }

  if (data.thermals) {
    const sList = document.getElementById('sensor-list');
    sList.innerHTML = '';
    (data.thermals.sensors || []).forEach((s) => {
      const row = document.createElement('div');
      row.className = 'telemetry-row';
      row.innerHTML = `<span>${s.name}</span><span class="telemetry-val">${s.temp}°C</span>`;
      sList.appendChild(row);
    });

    const fList = document.getElementById('fan-list');
    fList.innerHTML = '';
    (data.thermals.fans || []).forEach((f) => {
      const row = document.createElement('div');
      row.className = 'telemetry-row';
      row.innerHTML = `<span>Fan #${f.id + 1}</span><span class="telemetry-val">${f.rpm} RPM (${f.percent}%)</span>`;
      fList.appendChild(row);
    });
  }
}

function applyFullState(data) {
  state.devices = data.devices || [];
  state.profiles = data.profiles || [];
  state.activeProfileId = data.activeProfileId || 0;
  if (data.settings) {
    state.settings = data.settings;
    updateLightsButton();
    document.getElementById('master-brightness').value = data.settings.brightness ?? 255;
    document.getElementById('master-brightness-val').textContent = `${Math.round(((data.settings.brightness ?? 255) / 255) * 100)}%`;
    document.getElementById('set-start-windows').checked = !!data.settings.startWindows;
    document.getElementById('set-start-minimized').checked = !!data.settings.startMinimized;
    document.getElementById('set-prof-switch').checked = !!data.settings.enableProfSwitch;
    document.getElementById('set-gamma').checked = !!data.settings.gammaCorrection;
    document.getElementById('set-fan-control').checked = !!data.settings.fanControl;
    document.getElementById('set-off-battery').checked = !!data.settings.offOnBattery;
    document.getElementById('set-dim-battery').checked = !!data.settings.dimmedBatt;
  }

  // Update device dropdown (preserving currently selected device)
  const dd = document.getElementById('hw-device-dropdown');
  const hasSelected = state.devices.some((d) => d.pid === state.selectedDevicePid);
  if (!hasSelected) {
    state.selectedDevicePid = state.devices.length > 0 ? state.devices[0].pid : 0;
  }
  dd.innerHTML = '';
  state.devices.forEach((d) => {
    const opt = document.createElement('option');
    opt.value = d.pid;
    opt.textContent = d.name;
    dd.appendChild(opt);
  });
  if (state.selectedDevicePid) {
    dd.value = String(state.selectedDevicePid);
  }
  renderHardwareLights();

  // Update profiles
  const pGrid = document.getElementById('profiles-grid');
  pGrid.innerHTML = '';
  state.profiles.forEach((p) => {
    if (p.active) document.getElementById('hud-profile').textContent = p.name;
    const card = document.createElement('div');
    card.className = `profile-card ${p.active ? 'active' : ''}`;
    card.innerHTML = `
      <div>
        <strong>${p.name}</strong>
        <div class="hint-text">${p.isDefault ? 'System Default' : 'Custom Profile'}</div>
      </div>
      <div class="profile-actions">
        <button class="btn-secondary" onclick="activateProf(${p.id})">${p.active ? 'Active' : 'Activate'}</button>
        ${!p.isDefault ? `<button class="qs-btn" onclick="deleteProf(${p.id})">Delete</button>` : ''}
      </div>
    `;
    pGrid.appendChild(card);
  });
}

function renderHardwareLights() {
  const list = document.getElementById('hw-light-list');
  list.innerHTML = '';
  const dev = state.devices.find((d) => d.pid === state.selectedDevicePid) || state.devices[0];
  if (!dev) {
    list.innerHTML = '<div class="hint-text">No AlienFX hardware detected (Preview Mode).</div>';
    return;
  }
  (dev.lights || []).forEach((l) => {
    const row = document.createElement('div');
    row.className = `hw-light-item ${state.selectedHwLights.has(l.id) ? 'selected' : ''}`;
    row.dataset.lid = l.id;
    row.style.cursor = 'pointer';
    row.innerHTML = `<span>#${l.id} ${l.name || 'Unnamed LED'}</span><button class="hw-blink-btn">Blink</button>`;
    row.addEventListener('click', (e) => {
      if (e.target.closest('.hw-blink-btn')) return;
      if (!e.ctrlKey && !e.shiftKey) state.selectedHwLights.clear();
      if (state.selectedHwLights.has(l.id)) state.selectedHwLights.delete(l.id);
      else state.selectedHwLights.add(l.id);
      refreshSelectionUI();
    });
    row.querySelector('.hw-blink-btn').addEventListener('click', () => {
      sendCommand({ cmd: 'blinkLight', pid: dev.pid, lightId: l.id });
    });
    list.appendChild(row);
  });
}

window.activateProf = (id) => sendCommand({ cmd: 'activateProfile', id });
window.deleteProf = (id) => sendCommand({ cmd: 'deleteProfile', id });

function initWindowControls() {
  const minBtn = document.getElementById('win-min');
  const maxBtn = document.getElementById('win-max');
  const closeBtn = document.getElementById('win-close');
  const titlebar = document.getElementById('titlebar');

  if (minBtn) minBtn.addEventListener('click', () => sendCommand({ cmd: 'windowMinimize' }));
  if (maxBtn) maxBtn.addEventListener('click', () => sendCommand({ cmd: 'windowMaximize' }));
  if (closeBtn) closeBtn.addEventListener('click', () => sendCommand({ cmd: 'windowClose' }));

  if (titlebar) {
    titlebar.addEventListener('mousedown', (e) => {
      if (e.button !== 0) return;
      if (e.target.closest('button, input, select, a, .hud-pill, .window-controls')) return;
      sendCommand({ cmd: 'windowDrag' });
    });
    titlebar.addEventListener('dblclick', (e) => {
      if (e.target.closest('button, input, select, a, .hud-pill, .window-controls')) return;
      sendCommand({ cmd: 'windowMaximize' });
    });
  }
}

function initMiscActions() {
  document.getElementById('hw-device-dropdown').addEventListener('change', (e) => {
    state.selectedDevicePid = parseInt(e.target.value, 10);
    state.selectedHwLights.clear();
    renderHardwareLights();
    refreshSelectionUI();
  });

  const btnSelectHwAll = document.getElementById('btn-select-hw-all');
  if (btnSelectHwAll) {
    btnSelectHwAll.addEventListener('click', () => {
      const dev = state.devices.find((d) => d.pid === state.selectedDevicePid) || state.devices[0];
      if (!dev || !dev.lights) return;
      state.selectedHwLights.clear();
      dev.lights.forEach((l) => state.selectedHwLights.add(l.id));
      refreshSelectionUI();
    });
  }

  document.getElementById('btn-create-profile').addEventListener('click', () => {
    const inp = document.getElementById('new-profile-name');
    if (inp.value.trim()) {
      sendCommand({ cmd: 'createProfile', name: inp.value.trim() });
      inp.value = '';
    }
  });

  document.querySelectorAll('#power-mode-bar .seg-btn').forEach((btn) => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('#power-mode-bar .seg-btn').forEach((b) => b.classList.remove('active'));
      btn.classList.add('active');
      sendCommand({ cmd: 'setPowerMode', mode: parseInt(btn.dataset.pm, 10) });
    });
  });

  document.getElementById('btn-save-curve').addEventListener('click', () => {
    sendCommand({ cmd: 'setFanCurve', fanId: 0, sensorId: 0, active: true, points: state.fanCurve });
  });

  document.getElementById('btn-refresh-batt').addEventListener('click', () => {
    sendCommand({ cmd: 'getTelemetry' });
  });

  const settingMap = {
    'set-start-windows': 'startWindows',
    'set-start-minimized': 'startMinimized',
    'set-prof-switch': 'enableProfSwitch',
    'set-gamma': 'gammaCorrection',
    'set-fan-control': 'fanControl',
    'set-off-battery': 'offOnBattery',
    'set-dim-battery': 'dimmedBatt'
  };

  Object.entries(settingMap).forEach(([elId, key]) => {
    document.getElementById(elId).addEventListener('change', (e) => {
      sendCommand({ cmd: 'setSettings', [key]: e.target.checked });
    });
  });
}

document.addEventListener('DOMContentLoaded', () => {
  buildKeyboard();
  initNavigation();
  initLightingControls();
  initWindowControls();
  initMiscActions();
  renderFanCurve();

  if (hasWebView) {
    sendCommand({ cmd: 'getState' });
    sendCommand({ cmd: 'getTelemetry' });
  } else {
    // Demo data for browser preview
    applyFullState({
      devices: [{ pid: 0x550, name: 'Alienware Integrated Keyboard (Preview)', lights: [{ id: 1, name: 'WASD Cluster' }, { id: 2, name: 'Logo Emblem' }] }],
      profiles: [{ id: 0, name: 'Default', isDefault: true, active: true }, { id: 1, name: 'Orbital Gaming', isDefault: false, active: false }],
      settings: { lightsOn: true, brightness: 255, startWindows: true, startMinimized: true }
    });
    applyTelemetry({
      system: { cpu: 24, temp: 54, batt: 88 },
      batteries: [
        { name: 'System Battery', type: 'system', percentage: 88, charging: true, acOnline: true },
        { name: 'Wireless Gaming Mouse', type: 'mouse', percentage: 74, charging: false, acOnline: false }
      ],
      thermals: {
        sensors: [{ name: 'CPU Package', temp: 54 }, { name: 'GPU Core', temp: 48 }],
        fans: [{ id: 0, rpm: 2450, percent: 45 }, { id: 1, rpm: 2300, percent: 42 }]
      }
    });
  }
});
