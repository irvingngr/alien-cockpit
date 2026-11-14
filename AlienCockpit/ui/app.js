// AlienCockpit UI Controller
const hasWebView = !!(window.chrome && window.chrome.webview);

const state = {
  devices: [],
  profiles: [],
  activeProfileId: 0,
  settings: { lightsOn: true, brightness: 255, startWindows: false, startMinimized: true },
  selectedTargets: new Set(), // canonical key IDs or zone IDs
  selectedHwLights: new Set(), // direct physical LED IDs from Hardware LED Mapper
  selectedDevicePid: 0,
  activeView: 'keyboard',
  effect: 'static',
  colorPrimary: '#00f0ff',
  colorSecondary: '#7b2cbf',
  auroraColors: ['#00f0ff', '#00ff9d', '#7b2cbf'],
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

// ANSI Full Laptop Keyboard Layout matching physical Darfon 0x0D62 per-key LED IDs
const KEYBOARD_ROWS = [
  [
    { id: 'esc', label: 'ESC', lids: [0], vk: 27, aliases: ['esc', 'escape'], group: 'mods' },
    { id: 'f1', label: 'F1', lids: [1], vk: 112, aliases: ['f1'], group: 'fn' },
    { id: 'f2', label: 'F2', lids: [2], vk: 113, aliases: ['f2'], group: 'fn' },
    { id: 'f3', label: 'F3', lids: [3], vk: 114, aliases: ['f3'], group: 'fn' },
    { id: 'f4', label: 'F4', lids: [4], vk: 115, aliases: ['f4'], group: 'fn' },
    { id: 'f5', label: 'F5', lids: [5], vk: 116, aliases: ['f5'], group: 'fn' },
    { id: 'f6', label: 'F6', lids: [6], vk: 117, aliases: ['f6'], group: 'fn' },
    { id: 'f7', label: 'F7', lids: [7], vk: 118, aliases: ['f7'], group: 'fn' },
    { id: 'f8', label: 'F8', lids: [8], vk: 119, aliases: ['f8'], group: 'fn' },
    { id: 'f9', label: 'F9', lids: [9], vk: 120, aliases: ['f9'], group: 'fn' },
    { id: 'f10', label: 'F10', lids: [10], vk: 121, aliases: ['f10'], group: 'fn' },
    { id: 'f11', label: 'F11', lids: [11], vk: 122, aliases: ['f11'], group: 'fn' },
    { id: 'f12', label: 'F12', lids: [12], vk: 123, aliases: ['f12'], group: 'fn' },
    { id: 'home', label: 'HOME', lids: [13], vk: 292, aliases: ['home'], group: 'mods' },
    { id: 'end', label: 'END', lids: [14], vk: 291, aliases: ['end'], group: 'mods' },
    { id: 'del', label: 'DEL', lids: [15], vk: 302, aliases: ['del', 'delete'], group: 'mods' }
  ],
  [
    { id: 'tilde', label: '`', lids: [20], vk: 192, aliases: ['`', 'tilde'] },
    { id: '1', label: '1', lids: [21], vk: 49, aliases: ['1'] },
    { id: '2', label: '2', lids: [22], vk: 50, aliases: ['2'] },
    { id: '3', label: '3', lids: [23], vk: 51, aliases: ['3'] },
    { id: '4', label: '4', lids: [24], vk: 52, aliases: ['4'] },
    { id: '5', label: '5', lids: [25], vk: 53, aliases: ['5'] },
    { id: '6', label: '6', lids: [26], vk: 54, aliases: ['6'] },
    { id: '7', label: '7', lids: [27], vk: 55, aliases: ['7'] },
    { id: '8', label: '8', lids: [28], vk: 56, aliases: ['8'] },
    { id: '9', label: '9', lids: [29], vk: 57, aliases: ['9'] },
    { id: '0', label: '0', lids: [30], vk: 48, aliases: ['0'] },
    { id: 'minus', label: '-', lids: [31], vk: 189, aliases: ['-'] },
    { id: 'equals', label: '=', lids: [32], vk: 187, aliases: ['='] },
    { id: 'bksp', label: 'BKSP', cls: 'w-2', lids: [34, 35, 33], vk: 8, aliases: ['backspace', 'backspace 2'], group: 'mods' }
  ],
  [
    { id: 'tab', label: 'TAB', cls: 'w-15', lids: [40, 41], vk: 9, aliases: ['tab', 'tab 2'], group: 'mods' },
    { id: 'q', label: 'Q', lids: [42], vk: 81, aliases: ['q'] },
    { id: 'w', label: 'W', lids: [43], vk: 87, aliases: ['w'], group: 'wasd' },
    { id: 'e', label: 'E', lids: [44], vk: 69, aliases: ['e'] },
    { id: 'r', label: 'R', lids: [45], vk: 82, aliases: ['r'] },
    { id: 't', label: 'T', lids: [46], vk: 84, aliases: ['t'] },
    { id: 'y', label: 'Y', lids: [47], vk: 89, aliases: ['y'] },
    { id: 'u', label: 'U', lids: [48], vk: 85, aliases: ['u'] },
    { id: 'i', label: 'I', lids: [49], vk: 73, aliases: ['i'] },
    { id: 'o', label: 'O', lids: [50], vk: 79, aliases: ['o'] },
    { id: 'p', label: 'P', lids: [51], vk: 80, aliases: ['p'] },
    { id: 'lbracket', label: '[', lids: [52], vk: 219, aliases: ['['] },
    { id: 'rbracket', label: ']', lids: [53], vk: 221, aliases: [']'] },
    { id: 'backslash', label: '\\', cls: 'w-15', lids: [55, 54], vk: 220, aliases: ['\\', '\\ 2'] }
  ],
  [
    { id: 'caps', label: 'CAPS', cls: 'w-175', lids: [60, 61], vk: 20, aliases: ['caps lock', 'caps lock 2'], group: 'mods' },
    { id: 'a', label: 'A', lids: [62], vk: 65, aliases: ['a'], group: 'wasd' },
    { id: 's', label: 'S', lids: [63], vk: 83, aliases: ['s'], group: 'wasd' },
    { id: 'd', label: 'D', lids: [64], vk: 68, aliases: ['d'], group: 'wasd' },
    { id: 'f', label: 'F', lids: [65], vk: 70, aliases: ['f'] },
    { id: 'g', label: 'G', lids: [66], vk: 71, aliases: ['g'] },
    { id: 'h', label: 'H', lids: [67], vk: 72, aliases: ['h'] },
    { id: 'j', label: 'J', lids: [68], vk: 74, aliases: ['j'] },
    { id: 'k', label: 'K', lids: [69], vk: 75, aliases: ['k'] },
    { id: 'l', label: 'L', lids: [70], vk: 76, aliases: ['l'] },
    { id: 'semicolon', label: ';', lids: [71], vk: 186, aliases: [';'] },
    { id: 'quote', label: "'", lids: [72], vk: 222, aliases: ["'"] },
    { id: 'enter', label: 'ENTER', cls: 'w-225', lids: [74, 73, 75], vk: 13, aliases: ['enter', 'enter 2'], group: 'mods' }
  ],
  [
    { id: 'lshift', label: 'SHIFT', cls: 'w-225', lids: [80, 81, 82], vk: 160, aliases: ['shift', 'shift 2', 'left shift'], group: 'mods' },
    { id: 'z', label: 'Z', lids: [83], vk: 90, aliases: ['z'] },
    { id: 'x', label: 'X', lids: [84], vk: 88, aliases: ['x'] },
    { id: 'c', label: 'C', lids: [85], vk: 67, aliases: ['c'] },
    { id: 'v', label: 'V', lids: [86], vk: 86, aliases: ['v'] },
    { id: 'b', label: 'B', lids: [87], vk: 66, aliases: ['b'] },
    { id: 'n', label: 'N', lids: [88], vk: 78, aliases: ['n'] },
    { id: 'm', label: 'M', lids: [89], vk: 77, aliases: ['m'] },
    { id: 'comma', label: ',', lids: [90], vk: 188, aliases: [','] },
    { id: 'period', label: '.', lids: [91], vk: 190, aliases: ['.'] },
    { id: 'slash', label: '/', lids: [92], vk: 191, aliases: ['/'] },
    { id: 'rshift', label: 'RSHIFT', cls: 'w-175', lids: [94, 93], vk: 417, aliases: ['right shift', 'right shift 2'], group: 'mods' },
    { id: 'up', label: '▲', lids: [114], vk: 294, aliases: ['up'], group: 'arrows' }
  ],
  [
    { id: 'lctrl', label: 'CTRL', cls: 'w-125', lids: [100], vk: 162, aliases: ['ctrl', 'lctrl', 'left ctrl'], group: 'mods' },
    { id: 'fn', label: 'FN', lids: [101], vk: 255, aliases: ['fn'], group: 'mods' },
    { id: 'lwin', label: 'WIN', lids: [102, 103], vk: 347, aliases: ['left windows', 'left windows 2', 'win'], group: 'mods' },
    { id: 'lalt', label: 'ALT', lids: [104], vk: 164, aliases: ['alt', 'lalt', 'left alt'], group: 'mods' },
    { id: 'space', label: 'SPACE', cls: 'w-55', lids: [106, 107, 105, 108], vk: 32, aliases: ['space', 'space 2', 'space 3', 'space 4'] },
    { id: 'ralt', label: 'RALT', lids: [111], vk: 421, aliases: ['right alt', 'ralt'], group: 'mods' },
    { id: 'copilot', label: 'CP', cls: 'w-125', lids: [109, 112], vk: 348, aliases: ['right windows', 'right ctrl', 'rctrl', 'copilot', 'win lock', 'menu'], group: 'mods' },
    { id: 'left', label: '◀', lids: [133], vk: 293, aliases: ['left'], group: 'arrows' },
    { id: 'down', label: '▼', lids: [134], vk: 296, aliases: ['down'], group: 'arrows' },
    { id: 'right', label: '▶', lids: [135], vk: 295, aliases: ['right'], group: 'arrows' }
  ]
];

// 6-row Numpad including top media keys (aligned flush with main keyboard)
const NUMPAD_KEYS = [
  { id: 'mute', label: 'MUTE', lids: [16], vk: 173, aliases: ['mute', 'mic mute'], name: 'Mute', group: 'numpad' },
  { id: 'vol_dn', label: 'VOL-', lids: [17], vk: 430, aliases: ['volumedn', 'vol down', 'vol-'], name: 'Volume -', group: 'numpad' },
  { id: 'vol_up', label: 'VOL+', lids: [18], vk: 431, aliases: ['volumeup', 'vol up', 'vol+'], name: 'Volume +', group: 'numpad' },
  { id: 'mic', label: 'MIC', lids: [19], vk: 0, aliases: ['microphone', 'mic', 'mic mute'], name: 'Mic / Aux', group: 'numpad' },
  { id: 'num_lock', label: 'NUM', lids: [36], vk: 400, aliases: ['num lock', 'numlock'], name: 'Num Lock', group: 'numpad' },
  { id: 'num_div', label: '/', lids: [37], vk: 367, aliases: ['num /'], name: 'Num /', group: 'numpad' },
  { id: 'num_mul', label: '*', lids: [38], vk: 106, aliases: ['num *'], name: 'Num *', group: 'numpad' },
  { id: 'num_sub', label: '-', lids: [39], vk: 109, aliases: ['num -'], name: 'Num -', group: 'numpad' },
  { id: 'num_7', label: '7', lids: [56], vk: 103, aliases: ['num 7'], name: 'Num 7', group: 'numpad' },
  { id: 'num_8', label: '8', lids: [57], vk: 104, aliases: ['num 8'], name: 'Num 8', group: 'numpad' },
  { id: 'num_9', label: '9', lids: [58], vk: 105, aliases: ['num 9'], name: 'Num 9', group: 'numpad' },
  { id: 'num_add', label: '+', cls: 'num-tall', lids: [79, 59], vk: 107, aliases: ['num +', 'num + 2'], name: 'Num +', group: 'numpad' },
  { id: 'num_4', label: '4', lids: [76], vk: 100, aliases: ['num 4'], name: 'Num 4', group: 'numpad' },
  { id: 'num_5', label: '5', lids: [77], vk: 101, aliases: ['num 5'], name: 'Num 5', group: 'numpad' },
  { id: 'num_6', label: '6', lids: [78], vk: 102, aliases: ['num 6'], name: 'Num 6', group: 'numpad' },
  { id: 'num_1', label: '1', lids: [96], vk: 97, aliases: ['num 1'], name: 'Num 1', group: 'numpad' },
  { id: 'num_2', label: '2', lids: [97], vk: 98, aliases: ['num 2'], name: 'Num 2', group: 'numpad' },
  { id: 'num_3', label: '3', lids: [98], vk: 99, aliases: ['num 3'], name: 'Num 3', group: 'numpad' },
  { id: 'num_ent', label: 'ENT', cls: 'num-tall', lids: [99, 119], vk: 269, aliases: ['num enter', 'num enter 2'], name: 'Num Enter', group: 'numpad' },
  { id: 'num_0', label: '0', cls: 'num-wide', lids: [116, 117], vk: 96, aliases: ['num 0', 'num 0 2'], name: 'Num 0', group: 'numpad' },
  { id: 'num_dot', label: '.', lids: [118], vk: 110, aliases: ['num del .', 'num .'], name: 'Num .', group: 'numpad' }
];

const ALL_KEY_DEFS = [...KEYBOARD_ROWS.flat(), ...NUMPAD_KEYS];
const KEY_BY_ID = new Map(ALL_KEY_DEFS.map((k) => [k.id, k]));

function isKeyboardDevice(dev) {
  if (!dev) return false;
  return dev.vid === 0x0d62 || dev.version === 5 || (dev.lights && dev.lights.length > 20) ||
    /key|kbd|darfon/i.test(dev.name || '');
}

function isMouseDevice(dev) {
  if (!dev) return false;
  return /mouse|aw\d*m|310m|510m|610m|620m|720m|920m/i.test(dev.name || '');
}

function isChassisDevice(dev) {
  if (!dev) return false;
  return /elc|chassis|tron|lightbar|laptop|aurora|desktop/i.test(dev.name || '') ||
    (!isKeyboardDevice(dev) && !isMouseDevice(dev));
}

function isPerKeyKeyboard(dev) {
  return !!(dev && dev.lights && dev.lights.length > 20);
}

function getTargetDeviceForView(view = state.activeView) {
  const cur = state.devices.find((d) => d.pid === state.selectedDevicePid);
  const matcher = view === 'keyboard' ? isKeyboardDevice : view === 'mouse' ? isMouseDevice : isChassisDevice;
  if (cur && matcher(cur)) return cur;

  const presentMatch = state.devices.find((d) => d.present && matcher(d));
  if (presentMatch) return presentMatch;

  const anyMatch = state.devices.find((d) => matcher(d));
  if (anyMatch) return anyMatch;

  return view === 'keyboard' ? (cur || state.devices[0] || null) : null;
}

function findLightsForKey(keyDef, dev) {
  if (!keyDef || !dev || !dev.lights || dev.lights.length === 0) return [];
  const lights = dev.lights;
  const matched = [];
  const byId = new Set(lights.map((l) => l.id));

  // 1. Direct Darfon / per-key physical LED ID match
  if (isPerKeyKeyboard(dev) || dev.vid === 0x0d62 || dev.version === 5) {
    (keyDef.lids || []).forEach((lid) => {
      if (byId.has(lid)) matched.push(lid);
    });
    if (matched.length > 0) return Array.from(new Set(matched));
  }

  // 2. Exact normalized alias match (prevents 'f' matching 'fn' or 'f1')
  const aliases = new Set((keyDef.aliases || [keyDef.label]).map((s) => s.trim().toLowerCase()));
  lights.forEach((l) => {
    const lname = (l.name || '').trim().toLowerCase();
    if (lname && aliases.has(lname)) matched.push(l.id);
  });
  if (matched.length > 0) return Array.from(new Set(matched));

  // 3. Windows extended VK scancode fallback with FN/'F' collision guard
  if (keyDef.vk > 0) {
    lights.forEach((l) => {
      if (l.scancode !== keyDef.vk) return;
      const lname = (l.name || '').trim().toLowerCase();
      if (keyDef.id === 'fn' && lname === 'f') return;
      if (keyDef.id === 'f' && lname === 'fn') return;
      matched.push(l.id);
    });
  }
  return Array.from(new Set(matched));
}

function findTouchpadLights() {
  const hits = [];
  const ordered = [...state.devices].sort((a, b) => (isKeyboardDevice(a) ? 1 : 0) - (isKeyboardDevice(b) ? 1 : 0));
  ordered.forEach((dev) => {
    (dev.lights || []).forEach((l) => {
      if (/touchpad|trackpad|\bpad\b/i.test(l.name || '')) {
        hits.push({ pid: dev.pid, lid: l.id });
      }
    });
  });
  return hits;
}

function findLightsForZone(zoneId, zoneName, dev) {
  if (!dev || !dev.lights || dev.lights.length === 0) return [];
  const nonInd = dev.lights.filter((l) => !(l.flags & 3));
  const usable = nonInd.length > 0 ? nonInd : dev.lights;
  const numZid = parseInt(zoneId, 10);

  const kwRules = [
    [/scroll|wheel/i, /scroll|wheel/i],
    [/dpi/i, /dpi/i],
    [/palm|lid|emblem/i, /logo|emblem|head|palm|lid/i],
    [/rear|exhaust/i, /rear|exhaust|tron|back/i],
    [/front|lightbar/i, /front|lightbar|bar/i],
    [/underglow/i, /underglow|bottom|rear/i],
    [/left/i, /left/i],
    [/right/i, /right/i]
  ];
  for (const [zRe, lRe] of kwRules) {
    if (zRe.test(zoneName || '')) {
      const nameHits = usable.filter((l) => lRe.test(l.name || '')).map((l) => l.id);
      if (nameHits.length > 0) return nameHits;
    }
  }

  if (!Number.isNaN(numZid)) {
    const exact = dev.lights.find((l) => l.id === numZid);
    if (exact) return [exact.id];
    const baseIdx = numZid >= 10 ? numZid - 10 : numZid;
    return [usable[baseIdx % usable.length].id];
  }
  return [];
}

function applyKeyVisualStyle(el, color) {
  if (!el) return;
  if (color) {
    el.style.boxShadow = `inset 0 -3px 0 ${color}, 0 0 12px ${color}55`;
    el.style.borderColor = `${color}88`;
  } else {
    el.style.boxShadow = '';
    el.style.borderColor = '';
  }
}

function applyTouchpadVisualStyle(tpEl, color) {
  if (!tpEl) return;
  if (color) {
    tpEl.style.borderColor = color;
    tpEl.style.boxShadow = `0 0 18px ${color}55, inset 0 0 18px ${color}33`;
  } else {
    tpEl.style.borderColor = 'rgba(0, 240, 255, 0.25)';
    tpEl.style.boxShadow = 'none';
  }
}

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
      btn.dataset.name = key.name || key.label;
      if (key.group) btn.dataset.group = key.group;

      applyKeyVisualStyle(btn, state.keyColors[key.id] || '#00f0ff');

      btn.addEventListener('mousedown', (e) => {
        state.isDraggingSelect = true;
        state.selectedHwLights.clear();
        if (!e.ctrlKey && !e.shiftKey) state.selectedTargets.clear();
        toggleTargetSelection(key.id);
      });

      btn.addEventListener('mouseenter', () => {
        if (state.isDraggingSelect) {
          state.selectedTargets.add(key.id);
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

function buildNumpad() {
  const container = document.getElementById('kbd-numpad');
  if (!container) return;
  container.innerHTML = '';

  NUMPAD_KEYS.forEach((key) => {
    const btn = document.createElement('div');
    btn.className = `keycap ${key.cls || ''}`;
    btn.textContent = key.label;
    btn.dataset.id = key.id;
    btn.dataset.name = key.name || `Num ${key.label}`;
    btn.dataset.group = key.group || 'numpad';

    applyKeyVisualStyle(btn, state.keyColors[key.id] || '#00f0ff');

    btn.addEventListener('mousedown', (e) => {
      state.isDraggingSelect = true;
      state.selectedHwLights.clear();
      if (!e.ctrlKey && !e.shiftKey) state.selectedTargets.clear();
      toggleTargetSelection(key.id);
    });

    btn.addEventListener('mouseenter', () => {
      if (state.isDraggingSelect) {
        state.selectedTargets.add(key.id);
        refreshSelectionUI();
      }
    });

    container.appendChild(btn);
  });
}

function toggleTargetSelection(id) {
  const key = String(id);
  if (state.selectedTargets.has(key)) state.selectedTargets.delete(key);
  else state.selectedTargets.add(key);
  refreshSelectionUI();
}

function formatTargetLabel(t) {
  if (t === 'touchpad') return 'RGB Touchpad';
  const kd = KEY_BY_ID.get(t);
  if (kd) return kd.name || kd.label;
  const zEl = document.querySelector(`.mouse-zone[data-zone-id="${t}"], .chassis-zone[data-zone-id="${t}"]`);
  if (zEl && zEl.dataset.zoneName) return zEl.dataset.zoneName;
  return t;
}

function refreshSelectionUI() {
  document.querySelectorAll('.keycap').forEach((el) => {
    el.classList.toggle('selected', state.selectedTargets.has(el.dataset.id));
  });
  document.querySelectorAll('.mouse-zone, .chassis-zone, .touchpad-zone').forEach((el) => {
    el.classList.toggle('selected', state.selectedTargets.has(el.dataset.zoneId));
  });
  document.querySelectorAll('.hw-light-item').forEach((el) => {
    el.classList.toggle('selected', state.selectedHwLights.has(parseInt(el.dataset.lid, 10)));
  });

  const count = state.selectedTargets.size + state.selectedHwLights.size;
  document.getElementById('selected-count').textContent = `${count} selected`;
  const preview = document.getElementById('selected-names');
  if (count === 0) {
    preview.textContent = 'Click keys or zones on the left';
  } else if (state.selectedHwLights.size > 0) {
    preview.textContent = `Hardware LEDs: ${Array.from(state.selectedHwLights).slice(0, 8).map((id) => `#${id}`).join(', ')}${state.selectedHwLights.size > 8 ? '…' : ''}`;
  } else {
    const names = Array.from(state.selectedTargets).slice(0, 8).map(formatTargetLabel);
    preview.textContent = `Targets: ${names.join(', ')}${count > 8 ? '…' : ''}`;
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
      state.selectedHwLights.clear();

      const viewDev = getTargetDeviceForView(state.activeView);
      if (viewDev) {
        state.selectedDevicePid = viewDev.pid;
        const dd = document.getElementById('hw-device-dropdown');
        if (dd) dd.value = String(viewDev.pid);
        renderHardwareLights();
      }
      if (state.activeView === 'mouse') {
        document.getElementById('mouse-zone-label').textContent = viewDev
          ? `Targeting ${viewDev.name} — Click any mouse RGB zone`
          : 'No compatible AlienFX gaming mouse detected';
      } else if (state.activeView === 'chassis') {
        document.getElementById('chassis-zone-label').textContent = viewDev
          ? `Targeting ${viewDev.name} — Click any chassis zone`
          : 'No chassis RGB controller detected';
      }

      refreshSelectionUI();
      syncVirtualLightingFromState();
    });
  });

  // Quick Select buttons
  document.querySelectorAll('.qs-btn[data-select]').forEach((btn) => {
    btn.addEventListener('click', () => {
      const mode = btn.dataset.select;
      state.selectedHwLights.clear();
      state.selectedTargets.clear();
      if (mode === 'touchpad') {
        state.selectedTargets.add('touchpad');
      } else if (mode !== 'clear') {
        document.querySelectorAll('.keycap').forEach((k) => {
          if (mode === 'all' || k.dataset.group === mode) {
            state.selectedTargets.add(k.dataset.id);
          }
        });
        if (mode === 'all' && findTouchpadLights().length > 0) {
          state.selectedTargets.add('touchpad');
        }
      }
      refreshSelectionUI();
    });
  });

  // Mouse, Chassis & Touchpad zones
  document.querySelectorAll('.mouse-zone, .chassis-zone, .touchpad-zone').forEach((zone) => {
    zone.addEventListener('click', (e) => {
      state.selectedHwLights.clear();
      if (!e.ctrlKey && !e.shiftKey) state.selectedTargets.clear();
      toggleTargetSelection(zone.dataset.zoneId);
      if (zone.classList.contains('mouse-zone')) {
        document.getElementById('mouse-zone-label').textContent = `Selected: ${zone.dataset.zoneName}`;
      } else if (zone.classList.contains('chassis-zone')) {
        document.getElementById('chassis-zone-label').textContent = `Selected: ${zone.dataset.zoneName}`;
      }
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
      const showSec = ['morph', 'breathing', 'pulse', 'aurora'].includes(state.effect);
      document.getElementById('secondary-color-wrap').style.display = showSec ? 'flex' : 'none';
      const auroraWrap = document.getElementById('aurora-palette-wrap');
      if (auroraWrap) auroraWrap.style.display = state.effect === 'aurora' ? 'flex' : 'none';
    });
  });

  document.querySelectorAll('.palette-btn').forEach((btn) => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.palette-btn').forEach((b) => b.classList.remove('active'));
      btn.classList.add('active');
      const cols = (btn.dataset.colors || '').split(',').map((s) => s.trim()).filter(Boolean);
      if (cols.length >= 2) {
        state.auroraColors = [cols[0], cols[1], cols[2] || cols[0]];
        state.colorPrimary = cols[0];
        state.colorSecondary = cols[1];
        document.getElementById('color-primary').value = cols[0];
        document.getElementById('hex-primary').value = cols[0].toUpperCase();
        document.getElementById('color-secondary').value = cols[1];
      }
    });
  });

  document.getElementById('effect-speed').addEventListener('input', (e) => {
    state.speed = parseInt(e.target.value, 10);
  });

  document.getElementById('btn-apply-lighting').addEventListener('click', () => {
    const hwTargets = Array.from(state.selectedHwLights);
    const uiTargets = Array.from(state.selectedTargets);
    if (uiTargets.length === 0 && hwTargets.length === 0) return;

    const commonPayload = {
      color: state.colorPrimary,
      color2: state.colorSecondary,
      color3: state.auroraColors[2] || state.colorPrimary,
      effect: state.effect,
      speed: state.speed
    };

    // Direct Hardware LED Mapper selection always goes to currently chosen dropdown device
    if (hwTargets.length > 0) {
      const dev = state.devices.find((d) => d.pid === state.selectedDevicePid) || state.devices[0];
      if (!dev) return;
      sendCommand({
        cmd: 'applyZoneColor',
        pid: dev.pid,
        lightIds: hwTargets,
        zoneName: `LEDs ${hwTargets.slice(0, 4).map((x) => `#${x}`).join(',')}`,
        ...commonPayload
      });
      return;
    }

    // Update UI visual preview ONLY for selected targets
    uiTargets.forEach((id) => {
      state.keyColors[id] = state.colorPrimary;
      const keyEl = document.querySelector(`.keycap[data-id="${id}"]`);
      if (keyEl) applyKeyVisualStyle(keyEl, state.colorPrimary);

      const tpEl = document.querySelector(`.touchpad-zone[data-zone-id="${id}"]`);
      if (tpEl) applyTouchpadVisualStyle(tpEl, state.colorPrimary);

      const zoneEl = document.querySelector(`.mouse-zone[data-zone-id="${id}"], .chassis-zone[data-zone-id="${id}"]`);
      if (zoneEl) {
        if (zoneEl.tagName.toLowerCase() === 'path') zoneEl.style.stroke = state.colorPrimary;
        else zoneEl.style.fill = state.colorPrimary;
      }
    });

    if (state.activeView === 'keyboard') {
      const hasTouchpad = uiTargets.includes('touchpad');
      const keyTargets = uiTargets.filter((id) => id !== 'touchpad');

      if (hasTouchpad) {
        const tpLights = findTouchpadLights();
        if (tpLights.length > 0) {
          const byPid = new Map();
          tpLights.forEach((t) => {
            if (!byPid.has(t.pid)) byPid.set(t.pid, []);
            byPid.get(t.pid).push(t.lid);
          });
          byPid.forEach((lids, pid) => {
            sendCommand({
              cmd: 'applyZoneColor',
              pid,
              lightIds: Array.from(new Set(lids)),
              zoneName: 'RGB Touchpad',
              ...commonPayload
            });
          });
        } else if (keyTargets.length === 0) {
          document.getElementById('selected-names').textContent = 'No dedicated RGB Touchpad LED exposed by hardware';
          return;
        }
      }

      if (keyTargets.length === 0) return;

      const kbdDev = getTargetDeviceForView('keyboard');
      if (!kbdDev || !kbdDev.lights || kbdDev.lights.length === 0) return;

      const matched = [];
      keyTargets.forEach((id) => {
        const keyDef = KEY_BY_ID.get(id);
        if (keyDef) {
          matched.push(...findLightsForKey(keyDef, kbdDev));
        }
      });

      let resolvedIds = Array.from(new Set(matched));
      if (resolvedIds.length === 0 && !isPerKeyKeyboard(kbdDev)) {
        const nonInd = kbdDev.lights.filter((l) => !(l.flags & 3));
        resolvedIds = (nonInd.length > 0 ? nonInd : kbdDev.lights).map((l) => l.id);
      }

      if (resolvedIds.length === 0) {
        document.getElementById('selected-names').textContent = 'No matching physical key LEDs found';
        return;
      }

      const labelPreview = keyTargets.slice(0, 4).map((id) => KEY_BY_ID.get(id)?.label || id).join(', ');
      sendCommand({
        cmd: 'applyZoneColor',
        pid: kbdDev.pid,
        lightIds: resolvedIds,
        zoneName: keyTargets.length > 4 ? `Keys (${keyTargets.length})` : `Keys ${labelPreview}`,
        ...commonPayload
      });
      return;
    }

    // Mouse or Chassis views
    const viewDev = getTargetDeviceForView(state.activeView);
    if (!viewDev || !viewDev.lights || viewDev.lights.length === 0) {
      const labelEl = document.getElementById(state.activeView === 'mouse' ? 'mouse-zone-label' : 'chassis-zone-label');
      if (labelEl) labelEl.textContent = `No compatible ${state.activeView} device detected`;
      return;
    }

    const matched = [];
    uiTargets.forEach((zid) => {
      const zEl = document.querySelector(`.${state.activeView}-zone[data-zone-id="${zid}"]`);
      const zName = zEl ? zEl.dataset.zoneName : '';
      matched.push(...findLightsForZone(zid, zName, viewDev));
    });
    const resolvedIds = Array.from(new Set(matched));
    if (resolvedIds.length === 0) return;

    sendCommand({
      cmd: 'applyZoneColor',
      pid: viewDev.pid,
      lightIds: resolvedIds,
      zoneName: `${state.activeView.toUpperCase()} Zone`,
      ...commonPayload
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
  const canvas = document.querySelector('.visual-stage');
  if (canvas) {
    canvas.style.opacity = state.settings.lightsOn ? '1' : '0.45';
  }
}

window.restartAsAdmin = () => sendCommand({ cmd: 'restartAsAdmin' });

function renderFanCurve() {
  const svg = document.getElementById('fan-curve-svg');
  const w = 640, h = 240, pad = 34;
  svg.innerHTML = '';

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
    document.getElementById('hud-cpu').textContent = `${data.system.cpu ?? 0}%`;
    document.getElementById('hud-temp').textContent = `${data.system.temp > 0 ? data.system.temp : '--'}°C`;
    document.getElementById('hud-batt').textContent = `${data.system.batt >= 0 ? data.system.batt : '--'}%`;
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
    if (typeof data.thermals.powerMode === 'number') {
      document.querySelectorAll('#power-mode-bar .seg-btn').forEach((b) => {
        b.classList.toggle('active', parseInt(b.dataset.pm, 10) === data.thermals.powerMode);
      });
    }

    const sList = document.getElementById('sensor-list');
    sList.innerHTML = '';
    const sensors = data.thermals.sensors || [];
    if (sensors.length === 0) {
      sList.innerHTML = `
        <div class="hint-text">No ACPI thermal sensors detected.</div>
        ${data.thermals.available === false ? '<button class="btn-secondary" style="margin-top:8px" onclick="restartAsAdmin()">Enable ACPI Control (Run as Admin)</button>' : ''}
      `;
    } else {
      sensors.forEach((s) => {
        const row = document.createElement('div');
        row.className = 'telemetry-row';
        row.innerHTML = `<span>${s.name}</span><span class="telemetry-val">${s.temp}°C</span>`;
        sList.appendChild(row);
      });
    }

    const fList = document.getElementById('fan-list');
    fList.innerHTML = '';
    const fans = data.thermals.fans || [];
    if (fans.length === 0) {
      fList.innerHTML = '<div class="hint-text">No ACPI cooling fans detected.</div>';
    } else {
      fans.forEach((f) => {
        const row = document.createElement('div');
        row.className = 'telemetry-row';
        row.innerHTML = `<span>Fan #${f.id + 1}</span><span class="telemetry-val">${f.rpm} RPM (${f.percent}%)</span>`;
        fList.appendChild(row);
      });
    }
  }
}

function setAllVirtualColor(col) {
  document.querySelectorAll('.keycap[data-id]').forEach((k) => {
    state.keyColors[k.dataset.id] = col;
    applyKeyVisualStyle(k, col);
  });
  document.querySelectorAll('.touchpad-zone').forEach((tp) => {
    applyTouchpadVisualStyle(tp, col);
  });
  document.querySelectorAll('.mouse-zone, .chassis-zone').forEach((z) => {
    if (z.tagName.toLowerCase() === 'path') z.style.stroke = col;
    else z.style.fill = col;
  });
}

function syncVirtualLightingFromState() {
  const activeProf = state.profiles.find((p) => p.active) || state.profiles[0];
  if (!activeProf || !activeProf.lightsets || activeProf.lightsets.length === 0) return;

  // Build exact (pid, lid) -> color lookup from active profile lightsets
  const litColorMap = new Map();
  const deviceFallbackColor = new Map();
  activeProf.lightsets.forEach((ls) => {
    const col = ls.color || '#00f0ff';
    (ls.lights || []).forEach((gl) => {
      litColorMap.set(`${gl.did}:${gl.lid}`, col);
      if (!deviceFallbackColor.has(gl.did)) deviceFallbackColor.set(gl.did, col);
    });
  });

  // 1. Sync Keyboard & Numpad keys accurately against keyboard device
  const kbdDev = getTargetDeviceForView('keyboard');
  const kbdPerKey = isPerKeyKeyboard(kbdDev);
  const kbdFallback = kbdDev ? deviceFallbackColor.get(kbdDev.pid) : null;

  document.querySelectorAll('.keycap[data-id]').forEach((keyEl) => {
    const keyDef = KEY_BY_ID.get(keyEl.dataset.id);
    let col = null;
    if (keyDef && kbdDev) {
      const lids = findLightsForKey(keyDef, kbdDev);
      for (const lid of lids) {
        const hit = litColorMap.get(`${kbdDev.pid}:${lid}`);
        if (hit) {
          col = hit;
          break;
        }
      }
      if (!col && !kbdPerKey && kbdFallback) {
        col = kbdFallback;
      }
    }
    if (col) {
      state.keyColors[keyEl.dataset.id] = col;
      applyKeyVisualStyle(keyEl, col);
    } else {
      delete state.keyColors[keyEl.dataset.id];
      applyKeyVisualStyle(keyEl, null);
    }
  });

  // 2. Sync RGB Touchpad
  const tpEl = document.getElementById('rgb-touchpad');
  if (tpEl) {
    let tpCol = null;
    findTouchpadLights().forEach((t) => {
      const hit = litColorMap.get(`${t.pid}:${t.lid}`);
      if (hit && !tpCol) tpCol = hit;
    });
    applyTouchpadVisualStyle(tpEl, tpCol);
  }

  // 3. Sync Mouse zones
  const mouseDev = getTargetDeviceForView('mouse');
  document.querySelectorAll('.mouse-zone').forEach((zoneEl) => {
    let col = null;
    if (mouseDev) {
      const lids = findLightsForZone(zoneEl.dataset.zoneId, zoneEl.dataset.zoneName, mouseDev);
      for (const lid of lids) {
        const hit = litColorMap.get(`${mouseDev.pid}:${lid}`);
        if (hit) { col = hit; break; }
      }
      if (!col) col = deviceFallbackColor.get(mouseDev.pid) || null;
    }
    if (col) {
      if (zoneEl.tagName.toLowerCase() === 'path') zoneEl.style.stroke = col;
      else zoneEl.style.fill = col;
    }
  });

  // 4. Sync Chassis zones
  const chassisDev = getTargetDeviceForView('chassis');
  document.querySelectorAll('.chassis-zone').forEach((zoneEl) => {
    let col = null;
    if (chassisDev) {
      const lids = findLightsForZone(zoneEl.dataset.zoneId, zoneEl.dataset.zoneName, chassisDev);
      for (const lid of lids) {
        const hit = litColorMap.get(`${chassisDev.pid}:${lid}`);
        if (hit) { col = hit; break; }
      }
      if (!col) col = deviceFallbackColor.get(chassisDev.pid) || null;
    }
    if (col) {
      if (zoneEl.tagName.toLowerCase() === 'path') zoneEl.style.stroke = col;
      else zoneEl.style.fill = col;
    }
  });
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

  const dd = document.getElementById('hw-device-dropdown');
  const hasSelected = state.devices.some((d) => d.pid === state.selectedDevicePid);
  if (!hasSelected) {
    const viewDev = getTargetDeviceForView(state.activeView);
    state.selectedDevicePid = viewDev ? viewDev.pid : (state.devices.length > 0 ? state.devices[0].pid : 0);
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
  syncVirtualLightingFromState();

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
  const lights = [...(dev.lights || [])].sort((a, b) => a.id - b.id);
  lights.forEach((l) => {
    const row = document.createElement('div');
    row.className = `hw-light-item ${state.selectedHwLights.has(l.id) ? 'selected' : ''}`;
    row.dataset.lid = l.id;
    row.style.cursor = 'pointer';
    row.innerHTML = `<span>#${l.id} ${l.name || 'Unnamed LED'}</span><button class="hw-blink-btn">Blink</button>`;
    row.addEventListener('click', (e) => {
      if (e.target.closest('.hw-blink-btn')) return;
      state.selectedTargets.clear();
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
    syncVirtualLightingFromState();
  });

  const btnSelectHwAll = document.getElementById('btn-select-hw-all');
  if (btnSelectHwAll) {
    btnSelectHwAll.addEventListener('click', () => {
      const dev = state.devices.find((d) => d.pid === state.selectedDevicePid) || state.devices[0];
      if (!dev || !dev.lights) return;
      state.selectedTargets.clear();
      state.selectedHwLights.clear();
      dev.lights.forEach((l) => state.selectedHwLights.add(l.id));
      refreshSelectionUI();
    });
  }

  const btnAddHwLed = document.getElementById('btn-add-hw-led');
  if (btnAddHwLed) {
    btnAddHwLed.addEventListener('click', () => {
      const inp = document.getElementById('hw-add-led-id');
      const lid = parseInt(inp ? inp.value : '', 10);
      if (Number.isNaN(lid) || lid < 0 || lid > 255) return;
      const dev = state.devices.find((d) => d.pid === state.selectedDevicePid) || state.devices[0];
      if (!dev) return;
      sendCommand({ cmd: 'addLight', pid: dev.pid, lightId: lid, name: `Custom LED ${lid}` });
      if (inp) inp.value = '';
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

  const thermalPresets = {
    quiet: {
      mode: 0,
      points: [{ temp: 35, boost: 10 }, { temp: 55, boost: 22 }, { temp: 70, boost: 40 }, { temp: 85, boost: 65 }, { temp: 95, boost: 85 }]
    },
    balanced: {
      mode: 1,
      points: [{ temp: 35, boost: 20 }, { temp: 50, boost: 40 }, { temp: 65, boost: 65 }, { temp: 80, boost: 88 }, { temp: 92, boost: 100 }]
    },
    gaming: {
      mode: 2,
      points: [{ temp: 30, boost: 35 }, { temp: 48, boost: 58 }, { temp: 62, boost: 80 }, { temp: 75, boost: 95 }, { temp: 88, boost: 100 }]
    },
    max: {
      mode: 3,
      points: [{ temp: 25, boost: 80 }, { temp: 45, boost: 90 }, { temp: 60, boost: 100 }, { temp: 75, boost: 100 }, { temp: 90, boost: 100 }]
    }
  };

  document.querySelectorAll('[data-tpreset]').forEach((btn) => {
    btn.addEventListener('click', () => {
      const preset = thermalPresets[btn.dataset.tpreset];
      if (!preset) return;
      state.fanCurve = preset.points.map((p) => ({ ...p }));
      renderFanCurve();
      document.querySelectorAll('#power-mode-bar .seg-btn').forEach((b) => {
        b.classList.toggle('active', parseInt(b.dataset.pm, 10) === preset.mode);
      });
      sendCommand({ cmd: 'setPowerMode', mode: preset.mode });
      sendCommand({ cmd: 'setFanCurve', fanId: 0, sensorId: 0, active: true, points: state.fanCurve });
    });
  });

  const gamePresets = {
    cyberpunk: { color: '#fcee0a', color2: '#00f0ff', color3: '#ff007f', effect: 'aurora', speed: 60, name: 'Cyberpunk 2077' },
    aurora: { color: '#00f0ff', color2: '#00ff9d', color3: '#7b2cbf', effect: 'aurora', speed: 45, name: 'Aurora Borealis' },
    area51: { color: '#00f0ff', color2: '#7b2cbf', color3: '#3a86ff', effect: 'morph', speed: 50, name: 'Area-51 Orbital' },
    stealth: { color: '#ff2a4b', color2: '#7b2cbf', color3: '#ff2a4b', effect: 'breathing', speed: 35, name: 'Crimson Stealth' },
    spectrum: { color: '#ff007f', color2: '#3a86ff', color3: '#00ff9d', effect: 'spectrum', speed: 55, name: 'Rainbow Spectrum' }
  };

  document.querySelectorAll('#game-presets .preset-chip').forEach((btn) => {
    btn.addEventListener('click', () => {
      const p = gamePresets[btn.dataset.preset];
      if (!p) return;
      state.colorPrimary = p.color;
      state.colorSecondary = p.color2;
      state.auroraColors = [p.color, p.color2, p.color3];
      state.effect = p.effect;
      state.speed = p.speed;
      document.getElementById('color-primary').value = p.color;
      document.getElementById('hex-primary').value = p.color.toUpperCase();
      document.getElementById('color-secondary').value = p.color2;
      document.getElementById('effect-speed').value = p.speed;
      document.querySelectorAll('.eff-btn').forEach((b) => b.classList.toggle('active', b.dataset.eff === p.effect));
      const auroraWrap = document.getElementById('aurora-palette-wrap');
      if (auroraWrap) auroraWrap.style.display = p.effect === 'aurora' ? 'flex' : 'none';
      const showSec = ['morph', 'breathing', 'pulse', 'aurora'].includes(p.effect);
      document.getElementById('secondary-color-wrap').style.display = showSec ? 'flex' : 'none';

      setAllVirtualColor(p.color);

      state.devices.forEach((dev) => {
        if (!dev.lights || dev.lights.length === 0) return;
        const mask = isPerKeyKeyboard(dev) ? 1 : 3;
        const nonInd = dev.lights.filter((l) => !(l.flags & mask));
        const ids = (nonInd.length > 0 ? nonInd : dev.lights).map((l) => l.id);
        sendCommand({
          cmd: 'applyZoneColor',
          pid: dev.pid,
          lightIds: ids,
          color: p.color,
          color2: p.color2,
          color3: p.color3,
          effect: p.effect,
          speed: p.speed,
          zoneName: p.name
        });
      });
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
  buildNumpad();
  initNavigation();
  initLightingControls();
  initWindowControls();
  initMiscActions();
  renderFanCurve();

  if (hasWebView) {
    sendCommand({ cmd: 'getState' });
    sendCommand({ cmd: 'getTelemetry' });
  } else {
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
