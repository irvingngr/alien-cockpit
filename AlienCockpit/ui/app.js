// AlienCockpit UI Controller
const hasWebView = !!(window.chrome && window.chrome.webview);

const state = {
  devices: [],
  profiles: [],
  activeProfileId: 0,
  settings: { lightsOn: true, brightness: 255, startWindows: false, startMinimized: true, startupIntro: true },
  selectedTargets: new Set(), // canonical key IDs or zone IDs
  selectedHwLights: new Set(), // direct physical LED IDs from Hardware LED Mapper
  selectedDevicePid: 0,
  activeView: 'keyboard',
  effect: 'static',
  colorPrimary: '#00f0ff',
  colorSecondary: '#7b2cbf',
  auroraColors: ['#00f0ff', '#00ff9d', '#7b2cbf'],
  speed: 50,
  zoneBrightness: 100,
  probeActive: false,
  probeLedId: 0,
  keyColors: {},
  zoneColors: {},
  fanCurve: [
    { temp: 35, boost: 20 },
    { temp: 50, boost: 40 },
    { temp: 65, boost: 65 },
    { temp: 80, boost: 88 },
    { temp: 92, boost: 100 }
  ],
  fanMetrics: new Map(), // id -> { rpm, percent, angle }
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
    { id: 'space', label: 'SPACE', cls: 'w-45', lids: [106, 107, 105, 108], vk: 32, aliases: ['space', 'space 2', 'space 3', 'space 4'] },
    { id: 'ralt', label: 'RALT', lids: [111], vk: 421, aliases: ['right alt', 'ralt'], group: 'mods' },
    { id: 'rwin', label: 'WIN', lids: [109], vk: 348, aliases: ['right windows', 'rwin'], name: 'Right Windows', group: 'mods' },
    { id: 'copilot', label: 'CP', cls: 'w-125', lids: [112], vk: 419, aliases: ['right ctrl', 'rctrl', 'copilot', 'win lock', 'menu'], name: 'Copilot / RCtrl', group: 'mods' },
    { id: 'left', label: '◀', lids: [133], vk: 293, aliases: ['left'], group: 'arrows' },
    { id: 'down', label: '▼', lids: [134], vk: 296, aliases: ['down'], group: 'arrows' },
    { id: 'right', label: '▶', lids: [135], vk: 295, aliases: ['right'], group: 'arrows' }
  ]
];

// 6-row Numpad including top media keys & Power Button (aligned flush with main keyboard)
const NUMPAD_KEYS = [
  { id: 'mute', label: 'MUTE', lids: [16], vk: 173, aliases: ['mute', 'mic mute'], name: 'Mute', group: 'numpad' },
  { id: 'vol_dn', label: 'VOL-', lids: [17], vk: 430, aliases: ['volumedn', 'vol down', 'vol-'], name: 'Volume -', group: 'numpad' },
  { id: 'vol_up', label: 'VOL+', lids: [18], vk: 431, aliases: ['volumeup', 'vol up', 'vol+'], name: 'Volume +', group: 'numpad' },
  { id: 'power', label: '⏻ PWR', cls: 'key-power', lids: [], vk: 0, aliases: ['power', 'power button', 'pwr', 'power led', 'status power'], name: 'Power Button', group: 'power' },
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
  return dev.vid === 0x0461 || dev.version === 7 ||
    /mouse|aw\d*m|310m|510m|610m|620m|720m|920m/i.test(dev.name || '');
}

function isChassisDevice(dev) {
  if (!dev) return false;
  return dev.vid === 0x187c || dev.version === 4 ||
    /elc|chassis|tron|lightbar|lid|laptop|aurora|desktop/i.test(dev.name || '') ||
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

function getOrderedDevicesForPeripheralScan() {
  return [...state.devices].sort((a, b) => {
    const presDiff = (a.present === false ? 1 : 0) - (b.present === false ? 1 : 0);
    if (presDiff !== 0) return presDiff;
    return (isKeyboardDevice(a) ? 1 : 0) - (isKeyboardDevice(b) ? 1 : 0);
  });
}

function findTouchpadLights() {
  const hits = [];
  getOrderedDevicesForPeripheralScan().forEach((dev) => {
    (dev.lights || []).forEach((l) => {
      if (/touchpad|trackpad|mousepad|\bpad\b/i.test(l.name || '')) {
        hits.push({ pid: dev.pid, lid: l.id });
      }
    });
  });
  return hits;
}

function findPowerButtonLights() {
  const hits = [];
  getOrderedDevicesForPeripheralScan().forEach((dev) => {
    (dev.lights || []).forEach((l) => {
      if ((l.flags & 1) !== 0 || /power|pwr/i.test(l.name || '')) {
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
    [/strip|wheel|scroll|mouse light/i, /strip|wheel|scroll|mouse light/i],
    [/dpi/i, /dpi/i],
    [/mouse logo|palm|lid|emblem/i, /logo|emblem|head|palm|lid/i],
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
    // For multi-zone chassis views (10..14), map evenly across available chassis LEDs
    if (numZid >= 10 && usable.length > 0) {
      return [usable[(numZid - 10) % usable.length].id];
    }
  }
  return [];
}

function scaleHexColor(hex, pct = 100) {
  if (!hex || typeof hex !== 'string' || !/^#[0-9a-fA-F]{6}$/.test(hex)) return hex;
  const k = Math.max(0.05, Math.min(1, pct / 100));
  const r = Math.round(parseInt(hex.slice(1, 3), 16) * k);
  const g = Math.round(parseInt(hex.slice(3, 5), 16) * k);
  const b = Math.round(parseInt(hex.slice(5, 7), 16) * k);
  const toHex = (n) => Math.max(0, Math.min(255, n)).toString(16).padStart(2, '0');
  return `#${toHex(r)}${toHex(g)}${toHex(b)}`;
}

function triggerVirtualWaterRipple(originEl) {
  if (!originEl) return;
  const oRect = originEl.getBoundingClientRect();
  const ox = oRect.left + oRect.width / 2;
  const oy = oRect.top + oRect.height / 2;
  document.querySelectorAll('.keycap[data-id]').forEach((el) => {
    const r = el.getBoundingClientRect();
    const cx = r.left + r.width / 2;
    const cy = r.top + r.height / 2;
    const dist = Math.hypot(cx - ox, cy - oy);
    if (dist > 340) return;
    const delay = Math.round(dist * 1.15);
    setTimeout(() => {
      el.classList.add('water-ripple');
      setTimeout(() => el.classList.remove('water-ripple'), 260);
    }, delay);
  });
}

function applyKeyVisualStyle(el, color, effect = '') {
  if (!el) return;
  if (color) {
    if (effect === 'summer') {
      el.style.boxShadow = `inset 0 -3px 0 ${color}80, 0 0 10px ${color}44`;
      el.style.borderColor = `${color}66`;
    } else {
      el.style.boxShadow = `inset 0 -3px 0 ${color}, 0 0 12px ${color}55`;
      el.style.borderColor = `${color}88`;
    }
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
    tpEl.style.borderColor = '#2b3952';
    tpEl.style.boxShadow = 'none';
  }
}

function applySvgZoneVisualStyle(zoneEl, color) {
  if (!zoneEl) return;
  const paint = color || '#1e293b';
  const tag = zoneEl.tagName.toLowerCase();
  if (tag === 'g') {
    zoneEl.querySelectorAll('.mouse-paint').forEach((child) => { child.style.fill = paint; });
    zoneEl.querySelectorAll('.mouse-stroke').forEach((child) => { child.style.stroke = paint; });
  } else if (tag === 'path' || zoneEl.classList.contains('mouse-stroke')) {
    zoneEl.style.stroke = paint;
  } else {
    zoneEl.style.fill = paint;
  }
  const svg = zoneEl.closest('svg');
  if (color) {
    zoneEl.style.filter = (svg && svg.querySelector('#glow')) ? 'url(#glow)' : `drop-shadow(0 0 6px ${color})`;
  } else {
    zoneEl.style.filter = 'none';
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

      applyKeyVisualStyle(btn, state.keyColors[key.id] || null);

      btn.addEventListener('mousedown', (e) => {
        state.isDraggingSelect = true;
        state.selectedHwLights.clear();
        if (!e.ctrlKey && !e.shiftKey) state.selectedTargets.clear();
        toggleTargetSelection(key.id);
        if (state.effect === 'summer') {
          triggerVirtualWaterRipple(btn);
        }
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

    applyKeyVisualStyle(btn, state.keyColors[key.id] || null);

    btn.addEventListener('mousedown', (e) => {
      state.isDraggingSelect = true;
      state.selectedHwLights.clear();
      if (!e.ctrlKey && !e.shiftKey) state.selectedTargets.clear();
      toggleTargetSelection(key.id);
      if (state.effect === 'summer' && key.id !== 'power') {
        triggerVirtualWaterRipple(btn);
      }
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
  if (t === 'power') return 'Power Button';
  const kd = KEY_BY_ID.get(t);
  if (kd) return kd.name || kd.label;
  const zEl = document.querySelector(`.mouse-zone[data-zone-id="${t}"], .chassis-zone[data-zone-id="${t}"]`);
  if (zEl && zEl.dataset.zoneName) return zEl.dataset.zoneName;
  return t;
}

function updateUnmappedHelper() {
  const helper = document.getElementById('unmapped-helper');
  const txt = document.getElementById('unmapped-helper-text');
  if (!helper || !txt) return;

  if (state.selectedTargets.has('touchpad') && findTouchpadLights().length === 0) {
    txt.textContent = 'No RGB Touchpad LED is mapped yet. Activate Red LED Probe below, step ◀ ▶ until your touchpad lights up red, then click "→ Touchpad".';
    helper.style.display = 'flex';
  } else if (state.selectedTargets.has('power') && findPowerButtonLights().length === 0) {
    txt.textContent = 'No Power Button LED is mapped yet. Select your Chassis/Keyboard controller, activate Red LED Probe below, find your power LED, then click "→ Power Btn".';
    helper.style.display = 'flex';
  } else {
    helper.style.display = 'none';
  }
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
  updateUnmappedHelper();
}

function initNavigation() {
  document.querySelectorAll('.nav-item').forEach((btn) => {
    btn.addEventListener('click', () => {
      if (btn.dataset.tab !== 'lighting' && state.probeActive) {
        setProbeActive(false);
      }
      document.querySelectorAll('.nav-item').forEach((b) => b.classList.remove('active'));
      document.querySelectorAll('.tab-panel').forEach((p) => p.classList.remove('active'));
      btn.classList.add('active');
      document.getElementById(`tab-${btn.dataset.tab}`).classList.add('active');
    });
  });

  // Device view switcher (Keyboard / Mouse / Chassis)
  document.querySelectorAll('.device-switcher .seg-btn').forEach((btn) => {
    btn.addEventListener('click', () => {
      if (state.probeActive) setProbeActive(false);
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
        const pdd = document.getElementById('probe-dev-select');
        if (pdd) pdd.value = String(viewDev.pid);
        renderHardwareLights();
      }
      if (state.activeView === 'mouse') {
        document.getElementById('mouse-zone-label').textContent = viewDev
          ? `Targeting ${viewDev.name} — Click any mouse RGB zone`
          : 'No compatible AlienFX gaming mouse detected';
      } else if (state.activeView === 'chassis') {
        document.getElementById('chassis-zone-label').textContent = viewDev
          ? `Targeting ${viewDev.name} — Click any chassis zone (including Power Button)`
          : 'No chassis RGB controller detected';
      }

      refreshSelectionUI();
      updateProbeDeckUI();
      syncVirtualLightingFromState();
      syncInspectorFromView();
      syncLightingControlsForView();
    });
  });

  const btnStartVisualProbe = document.getElementById('btn-start-visual-probe');
  if (btnStartVisualProbe) {
    btnStartVisualProbe.addEventListener('click', () => {
      if (state.selectedTargets.has('touchpad')) {
        const chassisDev = state.devices.find((d) => d.present && isChassisDevice(d)) ||
          state.devices.find((d) => isChassisDevice(d));
        if (chassisDev && chassisDev.pid !== state.selectedDevicePid) {
          state.selectedDevicePid = chassisDev.pid;
          const dd = document.getElementById('hw-device-dropdown');
          if (dd) dd.value = String(chassisDev.pid);
          const pdd = document.getElementById('probe-dev-select');
          if (pdd) pdd.value = String(chassisDev.pid);
          renderHardwareLights();
          refreshSelectionUI();
          updateProbeDeckUI();
        }
      }
      setProbeActive(true);
    });
  }

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
          if (mode === 'all' && k.dataset.id === 'power') return;
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

function sendPowerButtonColor(colorHex) {
  const pwrLights = findPowerButtonLights();
  if (pwrLights.length === 0) {
    document.getElementById('selected-names').textContent = 'No Power Button LED found — use Red LED Probe below or Hardware LED Mapper & click "→ Power Btn"';
    return false;
  }
  const byPid = new Map();
  pwrLights.forEach((p) => {
    if (!byPid.has(p.pid)) byPid.set(p.pid, []);
    byPid.get(p.pid).push(p.lid);
  });
  byPid.forEach((lids, pid) => {
    sendCommand({
      cmd: 'applyZoneColor',
      pid,
      lightIds: Array.from(new Set(lids)),
      color: colorHex,
      color2: colorHex,
      effect: 'static',
      speed: 50,
      zoneBrightness: state.zoneBrightness,
      zoneName: 'Power Button'
    });
  });
  const visColor = scaleHexColor(colorHex, state.zoneBrightness);
  state.keyColors.power = visColor;
  const pwrKey = document.querySelector('.keycap[data-id="power"]');
  if (pwrKey) applyKeyVisualStyle(pwrKey, visColor);
  const pwrChassis = document.querySelector('.chassis-zone[data-zone-id="power"]');
  if (pwrChassis) applySvgZoneVisualStyle(pwrChassis, visColor);
  return true;
}

function syncLightingControlsForView() {
  const isMouse = state.activeView === 'mouse';
  document.querySelectorAll('#effect-grid .eff-btn').forEach((b) => {
    const allowed = !isMouse || b.dataset.eff === 'static' || b.dataset.eff === 'breathing';
    b.style.display = allowed ? '' : 'none';
  });
  if (isMouse && state.effect !== 'static' && state.effect !== 'breathing') {
    state.effect = 'static';
  }
  document.querySelectorAll('#effect-grid .eff-btn').forEach((b) => {
    b.classList.toggle('active', b.dataset.eff === state.effect);
  });
  const showSec = isMouse || ['morph', 'aurora', 'breathing'].includes(state.effect);
  const secWrap = document.getElementById('secondary-color-wrap');
  if (secWrap) secWrap.style.display = showSec ? 'flex' : 'none';
  const auroraWrap = document.getElementById('aurora-palette-wrap');
  if (auroraWrap) auroraWrap.style.display = (!isMouse && state.effect === 'aurora') ? 'flex' : 'none';
}

function initLightingControls() {
  const cPrimary = document.getElementById('color-primary');
  const hPrimary = document.getElementById('hex-primary');
  const cSecondary = document.getElementById('color-secondary');
  const zBright = document.getElementById('zone-brightness');
  const zBrightVal = document.getElementById('zone-brightness-val');

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

  if (zBright && zBrightVal) {
    zBright.addEventListener('input', () => {
      state.zoneBrightness = Math.max(5, Math.min(100, parseInt(zBright.value, 10) || 100));
      zBrightVal.textContent = `${state.zoneBrightness}%`;
    });
  }

  document.querySelectorAll('.swatch').forEach((sw) => {
    sw.addEventListener('click', () => {
      state.colorPrimary = sw.dataset.color;
      cPrimary.value = sw.dataset.color;
      hPrimary.value = sw.dataset.color.toUpperCase();
    });
  });

  document.querySelectorAll('.eff-btn').forEach((btn) => {
    btn.addEventListener('click', () => {
      if (state.activeView === 'mouse' && btn.dataset.eff !== 'static' && btn.dataset.eff !== 'breathing') {
        return;
      }
      state.effect = btn.dataset.eff;
      if (state.effect === 'summer') {
        state.colorPrimary = '#38bdf8';
        cPrimary.value = '#38bdf8';
        hPrimary.value = '#38BDF8';
      }
      syncLightingControlsForView();
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

    if (state.probeActive) {
      state.probeActive = false;
      const toggle = document.getElementById('probe-toggle');
      if (toggle) toggle.checked = false;
      updateProbeDeckUI();
    }

    const commonPayload = {
      color: state.colorPrimary,
      color2: state.colorSecondary,
      color3: state.auroraColors[2] || state.colorPrimary,
      effect: state.effect,
      speed: state.speed,
      zoneBrightness: state.zoneBrightness
    };
    const visColor = scaleHexColor(state.colorPrimary, state.zoneBrightness);

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

    const hasPower = uiTargets.includes('power');
    if (hasPower) {
      sendPowerButtonColor(state.colorPrimary);
    }

    if (state.activeView === 'keyboard') {
      const hasTouchpad = uiTargets.includes('touchpad');
      const keyTargets = uiTargets.filter((id) => id !== 'touchpad' && id !== 'power');

      if (hasTouchpad) {
        const tpLights = findTouchpadLights();
        if (tpLights.length > 0) {
          const tpEl = document.getElementById('rgb-touchpad');
          if (tpEl) applyTouchpadVisualStyle(tpEl, visColor);
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
        } else if (keyTargets.length === 0 && !hasPower) {
          document.getElementById('selected-names').textContent = 'No RGB Touchpad LED mapped yet — use Red LED Probe below & click "→ Touchpad"';
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
          const lids = findLightsForKey(keyDef, kbdDev);
          if (lids.length > 0 || !isPerKeyKeyboard(kbdDev)) {
            state.keyColors[id] = visColor;
            const keyEl = document.querySelector(`.keycap[data-id="${id}"]`);
            if (keyEl) applyKeyVisualStyle(keyEl, visColor, state.effect);
            matched.push(...lids);
          }
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

    // Mouse or Chassis views (non-power zones)
    const zoneTargets = uiTargets.filter((zid) => zid !== 'power');
    if (zoneTargets.length === 0) return;

    const viewDev = getTargetDeviceForView(state.activeView);
    if (!viewDev || !viewDev.lights || viewDev.lights.length === 0) {
      const labelEl = document.getElementById(state.activeView === 'mouse' ? 'mouse-zone-label' : 'chassis-zone-label');
      if (labelEl) labelEl.textContent = `No compatible ${state.activeView} device detected`;
      return;
    }

    const matched = [];
    zoneTargets.forEach((zid) => {
      const zEl = document.querySelector(`.${state.activeView}-zone[data-zone-id="${zid}"]`);
      const zName = zEl ? zEl.dataset.zoneName : '';
      const lids = findLightsForZone(zid, zName, viewDev);
      if (lids.length > 0 && zEl) {
        applySvgZoneVisualStyle(zEl, visColor);
        matched.push(...lids);
      }
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

const THERMAL_PRESETS = {
  quiet: {
    label: 'Silent',
    mode: 0,
    points: [{ temp: 35, boost: 10 }, { temp: 55, boost: 22 }, { temp: 70, boost: 40 }, { temp: 85, boost: 65 }, { temp: 95, boost: 85 }]
  },
  balanced: {
    label: 'Balanced',
    mode: 1,
    points: [{ temp: 35, boost: 20 }, { temp: 50, boost: 40 }, { temp: 65, boost: 65 }, { temp: 80, boost: 88 }, { temp: 92, boost: 100 }]
  },
  gaming: {
    label: 'Gaming',
    mode: 2,
    points: [{ temp: 30, boost: 35 }, { temp: 48, boost: 58 }, { temp: 62, boost: 80 }, { temp: 75, boost: 95 }, { temp: 88, boost: 100 }]
  },
  max: {
    label: 'Max Cooling',
    mode: 3,
    points: [{ temp: 25, boost: 80 }, { temp: 45, boost: 90 }, { temp: 60, boost: 100 }, { temp: 75, boost: 100 }, { temp: 90, boost: 100 }]
  }
};

function detectThermalPreset(points = state.fanCurve) {
  if (!Array.isArray(points) || points.length === 0) return 'Custom';
  for (const preset of Object.values(THERMAL_PRESETS)) {
    if (preset.points.length !== points.length) continue;
    const match = preset.points.every((pp, idx) =>
      Math.abs(pp.temp - points[idx].temp) <= 1 && Math.abs(pp.boost - points[idx].boost) <= 1
    );
    if (match) return preset.label;
  }
  return 'Custom';
}

function updateCurvePresetBadge() {
  const badge = document.getElementById('curve-active-badge');
  if (!badge) return;
  const label = detectThermalPreset(state.fanCurve);
  badge.textContent = label;
  badge.classList.toggle('custom', label === 'Custom');
}

const curveUi = {
  initialized: false,
  dragIdx: -1,
  svg: null,
  poly: null,
  area: null,
  guideV: null,
  guideH: null,
  tipGroup: null,
  tipRect: null,
  tipText: null,
  handles: [],
  hitTargets: []
};

function renderFanCurve() {
  const svg = document.getElementById('fan-curve-svg');
  if (!svg) return;
  const w = 640, h = 240, pad = 34;
  const toX = (t) => pad + ((t - 20) / 80) * (w - 2 * pad);
  const toY = (b) => h - pad - (b / 100) * (h - 2 * pad);

  if (!curveUi.initialized || curveUi.handles.length !== state.fanCurve.length) {
    svg.innerHTML = '';
    curveUi.svg = svg;
    curveUi.handles = [];
    curveUi.hitTargets = [];

    // Background horizontal & vertical grid
    for (let i = 0; i <= 4; i++) {
      const y = pad + ((h - 2 * pad) * i) / 4;
      const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
      line.setAttribute('x1', pad); line.setAttribute('x2', w - pad);
      line.setAttribute('y1', y); line.setAttribute('y2', y);
      line.setAttribute('stroke', 'rgba(255,255,255,0.07)');
      svg.appendChild(line);

      const lbl = document.createElementNS('http://www.w3.org/2000/svg', 'text');
      lbl.setAttribute('x', 4); lbl.setAttribute('y', y + 4);
      lbl.setAttribute('fill', '#64748b'); lbl.setAttribute('font-size', '10');
      lbl.textContent = `${100 - i * 25}%`;
      svg.appendChild(lbl);
    }
    for (let t = 20; t <= 100; t += 20) {
      const x = toX(t);
      const vline = document.createElementNS('http://www.w3.org/2000/svg', 'line');
      vline.setAttribute('x1', x); vline.setAttribute('x2', x);
      vline.setAttribute('y1', pad); vline.setAttribute('y2', h - pad);
      vline.setAttribute('stroke', 'rgba(255,255,255,0.05)');
      svg.appendChild(vline);

      const tlbl = document.createElementNS('http://www.w3.org/2000/svg', 'text');
      tlbl.setAttribute('x', x - 12); tlbl.setAttribute('y', h - 10);
      tlbl.setAttribute('fill', '#64748b'); tlbl.setAttribute('font-size', '10');
      tlbl.textContent = `${t}°C`;
      svg.appendChild(tlbl);
    }

    const area = document.createElementNS('http://www.w3.org/2000/svg', 'polygon');
    area.setAttribute('fill', 'rgba(0, 240, 255, 0.10)');
    svg.appendChild(area);
    curveUi.area = area;

    const poly = document.createElementNS('http://www.w3.org/2000/svg', 'polyline');
    poly.setAttribute('fill', 'none');
    poly.setAttribute('stroke', '#00f0ff');
    poly.setAttribute('stroke-width', '3');
    poly.setAttribute('stroke-linejoin', 'round');
    poly.setAttribute('stroke-linecap', 'round');
    svg.appendChild(poly);
    curveUi.poly = poly;

    const guideV = document.createElementNS('http://www.w3.org/2000/svg', 'line');
    guideV.setAttribute('y1', pad); guideV.setAttribute('y2', h - pad);
    guideV.setAttribute('stroke', 'rgba(0, 240, 255, 0.35)');
    guideV.setAttribute('stroke-dasharray', '4 3');
    guideV.style.display = 'none';
    svg.appendChild(guideV);
    curveUi.guideV = guideV;

    const guideH = document.createElementNS('http://www.w3.org/2000/svg', 'line');
    guideH.setAttribute('x1', pad); guideH.setAttribute('x2', w - pad);
    guideH.setAttribute('stroke', 'rgba(0, 240, 255, 0.35)');
    guideH.setAttribute('stroke-dasharray', '4 3');
    guideH.style.display = 'none';
    svg.appendChild(guideH);
    curveUi.guideH = guideH;

    state.fanCurve.forEach((_, idx) => {
      const vis = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
      vis.setAttribute('r', '7');
      vis.setAttribute('fill', '#05070d');
      vis.setAttribute('stroke', '#00f0ff');
      vis.setAttribute('stroke-width', '3');
      svg.appendChild(vis);
      curveUi.handles.push(vis);

      const hit = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
      hit.setAttribute('r', '18');
      hit.setAttribute('fill', 'transparent');
      hit.style.cursor = 'grab';

      const applyEventToPoint = (ev) => {
        const rect = svg.getBoundingClientRect();
        const rw = rect.width || w;
        const rh = rect.height || h;
        const relX = ((ev.clientX - rect.left) / rw) * w;
        const relY = ((ev.clientY - rect.top) / rh) * h;
        const minT = idx > 0 ? state.fanCurve[idx - 1].temp + 1 : 20;
        const maxT = idx < state.fanCurve.length - 1 ? state.fanCurve[idx + 1].temp - 1 : 100;
        const rawTemp = Math.round(20 + ((relX - pad) / (w - 2 * pad)) * 80);
        const temp = Math.max(minT, Math.min(maxT, rawTemp));
        const boost = Math.max(0, Math.min(100, Math.round((1 - (relY - pad) / (h - 2 * pad)) * 100)));
        state.fanCurve[idx] = { temp, boost };
        updateCurvePositions(idx);
        updateCurvePresetBadge();
      };

      const startDrag = (ev) => {
        if (ev.preventDefault) ev.preventDefault();
        curveUi.dragIdx = idx;
        hit.style.cursor = 'grabbing';
        if (typeof hit.setPointerCapture === 'function' && ev.pointerId !== undefined) {
          try { hit.setPointerCapture(ev.pointerId); } catch (_) {}
        }
        updateCurvePositions(idx);

        const onMove = (mev) => {
          if (curveUi.dragIdx !== idx) return;
          applyEventToPoint(mev);
        };
        const endDrag = (uev) => {
          curveUi.dragIdx = -1;
          hit.style.cursor = 'grab';
          if (typeof hit.releasePointerCapture === 'function' && uev && uev.pointerId !== undefined) {
            try { hit.releasePointerCapture(uev.pointerId); } catch (_) {}
          }
          curveUi.guideV.style.display = 'none';
          curveUi.guideH.style.display = 'none';
          curveUi.tipGroup.style.display = 'none';
          curveUi.handles.forEach((hc) => { hc.setAttribute('r', '7'); hc.setAttribute('fill', '#05070d'); });
          window.removeEventListener('pointermove', onMove);
          window.removeEventListener('pointerup', endDrag);
          window.removeEventListener('pointercancel', endDrag);
        };
        window.addEventListener('pointermove', onMove);
        window.addEventListener('pointerup', endDrag);
        window.addEventListener('pointercancel', endDrag);
      };

      hit.addEventListener('pointerdown', startDrag);
      svg.appendChild(hit);
      curveUi.hitTargets.push(hit);
    });

    const tipG = document.createElementNS('http://www.w3.org/2000/svg', 'g');
    tipG.style.display = 'none';
    tipG.style.pointerEvents = 'none';
    const tipR = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
    tipR.setAttribute('width', '88'); tipR.setAttribute('height', '22'); tipR.setAttribute('rx', '5');
    tipR.setAttribute('fill', '#05070d'); tipR.setAttribute('stroke', '#00f0ff'); tipR.setAttribute('stroke-width', '1');
    const tipT = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    tipT.setAttribute('x', '44'); tipT.setAttribute('y', '15');
    tipT.setAttribute('text-anchor', 'middle'); tipT.setAttribute('fill', '#e6eef8');
    tipT.setAttribute('font-size', '11'); tipT.setAttribute('font-family', 'Consolas, monospace');
    tipG.appendChild(tipR);
    tipG.appendChild(tipT);
    svg.appendChild(tipG);
    curveUi.tipGroup = tipG;
    curveUi.tipRect = tipR;
    curveUi.tipText = tipT;
    curveUi.initialized = true;
  }

  updateCurvePositions(curveUi.dragIdx);
  updateCurvePresetBadge();
}

function updateCurvePositions(activeIdx = -1) {
  if (!curveUi.initialized) return;
  const w = 640, h = 240, pad = 34;
  const toX = (t) => pad + ((t - 20) / 80) * (w - 2 * pad);
  const toY = (b) => h - pad - (b / 100) * (h - 2 * pad);

  const coords = state.fanCurve.map((p) => ({ x: toX(p.temp), y: toY(p.boost), ...p }));
  curveUi.poly.setAttribute('points', coords.map((c) => `${c.x},${c.y}`).join(' '));
  if (coords.length > 0) {
    const first = coords[0];
    const last = coords[coords.length - 1];
    const polyArea = `${first.x},${h - pad} ${coords.map((c) => `${c.x},${c.y}`).join(' ')} ${last.x},${h - pad}`;
    curveUi.area.setAttribute('points', polyArea);
  }

  coords.forEach((c, i) => {
    const vis = curveUi.handles[i];
    const hit = curveUi.hitTargets[i];
    if (vis) {
      vis.setAttribute('cx', c.x);
      vis.setAttribute('cy', c.y);
      const isActive = i === activeIdx;
      vis.setAttribute('r', isActive ? '9' : '7');
      vis.setAttribute('fill', isActive ? '#00f0ff' : '#05070d');
    }
    if (hit) {
      hit.setAttribute('cx', c.x);
      hit.setAttribute('cy', c.y);
    }
  });

  if (activeIdx >= 0 && coords[activeIdx]) {
    const pt = coords[activeIdx];
    curveUi.guideV.setAttribute('x1', pt.x); curveUi.guideV.setAttribute('x2', pt.x);
    curveUi.guideV.style.display = '';
    curveUi.guideH.setAttribute('y1', pt.y); curveUi.guideH.setAttribute('y2', pt.y);
    curveUi.guideH.style.display = '';

    const tx = Math.max(pad, Math.min(w - pad - 88, pt.x - 44));
    const ty = pt.y > pad + 32 ? pt.y - 30 : pt.y + 12;
    curveUi.tipGroup.setAttribute('transform', `translate(${tx}, ${ty})`);
    curveUi.tipText.textContent = `${pt.temp}°C • ${pt.boost}%`;
    curveUi.tipGroup.style.display = '';
  }
}

let fanSpinLoopStarted = false;
function ensureFanSpinLoop() {
  if (fanSpinLoopStarted || typeof window.requestAnimationFrame !== 'function') return;
  fanSpinLoopStarted = true;
  let lastTs = 0;
  const tick = (ts) => {
    const dt = lastTs ? Math.min(0.1, (ts - lastTs) / 1000) : 0.016;
    lastTs = ts;
    state.fanMetrics.forEach((m, id) => {
      const degPerSec = m.rpm > 0
        ? Math.max(95, Math.min(2400, m.rpm * 0.42))
        : (m.percent > 0 ? m.percent * 14 : 0);
      if (degPerSec > 0) {
        m.angle = (m.angle + degPerSec * dt) % 360;
        const rotor = document.querySelector(`.fan-row-visual[data-fan-id="${id}"] .fan-rotor-svg`);
        if (rotor) {
          rotor.style.transform = `rotate(${m.angle.toFixed(1)}deg)`;
        }
      }
    });
    window.requestAnimationFrame(tick);
  };
  window.requestAnimationFrame(tick);
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
    if (!data.batteries.some((b) => b.type !== 'system')) {
      const info = document.createElement('div');
      info.className = 'card';
      info.innerHTML = `
        <div class="batt-type">PERIPHERAL TELEMETRY INFO</div>
        <p class="hint-text" style="margin-top:6px;">No Bluetooth or standard HID Battery-Class peripheral reported a charge level. Note: Some Alienware 2.4GHz USB dongles (such as AW610M) use a proprietary vendor HID protocol rather than standard Windows HID battery reports.</p>
      `;
      grid.appendChild(info);
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
      if (!data.thermals.isAdmin) {
        sList.innerHTML = `
          <div class="hint-text">Administrator privileges are required to access ACPI BIOS thermal sensors and fan control.</div>
          <button class="btn-secondary" style="margin-top:8px" onclick="restartAsAdmin()">Relaunch as Administrator</button>
        `;
      } else {
        sList.innerHTML = `
          <div class="hint-text">Running as Administrator, but no compatible Alienware ACPI thermal interface was exposed on this hardware.</div>
        `;
      }
    } else {
      sensors.forEach((s) => {
        const row = document.createElement('div');
        row.className = 'telemetry-row';
        row.innerHTML = `<span>${s.name}</span><span class="telemetry-val">${s.temp}°C</span>`;
        sList.appendChild(row);
      });
    }

    const fList = document.getElementById('fan-list');
    const fans = data.thermals.fans || [];
    if (fans.length === 0) {
      state.fanMetrics.clear();
      fList.innerHTML = `<div class="hint-text">${!data.thermals.isAdmin ? 'Relaunch as Administrator to read cooling fan RPM.' : 'No ACPI cooling fans detected.'}</div>`;
    } else {
      const existingRows = Array.from(fList.querySelectorAll('.fan-row-visual'));
      const sameLayout = existingRows.length === fans.length &&
        fans.every((f, idx) => parseInt(existingRows[idx].dataset.fanId, 10) === f.id);

      if (!sameLayout) {
        fList.innerHTML = '';
      }

      fans.forEach((f, idx) => {
        const pct = Math.max(0, Math.min(100, Math.round(f.percent ?? 0)));
        const prev = state.fanMetrics.get(f.id) || { rpm: 0, percent: 0, angle: 0 };
        state.fanMetrics.set(f.id, { rpm: f.rpm || 0, percent: pct, angle: prev.angle });

        if (sameLayout) {
          const row = existingRows[idx];
          const bar = row.querySelector('.fan-bar-fill');
          const txt = row.querySelector('.fan-rpm-text');
          if (bar) bar.style.width = `${pct}%`;
          if (txt) txt.textContent = `${f.rpm} RPM (${pct}%)`;
        } else {
          const row = document.createElement('div');
          row.className = 'fan-row-visual';
          row.dataset.fanId = String(f.id);
          row.innerHTML = `
            <div class="fan-row-left">
              <div class="fan-rotor-wrap">
                <svg class="fan-rotor-svg" viewBox="0 0 36 36" aria-hidden="true">
                  <circle cx="18" cy="18" r="15" fill="none" stroke="rgba(0,240,255,0.22)" stroke-width="1"/>
                  <g class="fan-blades" fill="currentColor">
                    <path d="M18,18 C15,10 19,4 22,5 C24,6 22,13 18,18 Z"/>
                    <path d="M18,18 C25,13 31,15 31,18 C31,20 24,20 18,18 Z"/>
                    <path d="M18,18 C24,24 23,31 20,31 C18,31 16,25 18,18 Z"/>
                    <path d="M18,18 C12,25 5,23 5,20 C5,18 11,16 18,18 Z"/>
                    <path d="M18,18 C11,13 9,6 12,5 C14,4 16,10 18,18 Z"/>
                  </g>
                  <circle cx="18" cy="18" r="3.5" fill="#0b101d" stroke="currentColor" stroke-width="1.5"/>
                </svg>
              </div>
              <div class="fan-meta">
                <strong>Fan #${f.id + 1}</strong>
                <div class="fan-bar"><div class="fan-bar-fill" style="width:${pct}%"></div></div>
              </div>
            </div>
            <span class="telemetry-val fan-rpm-text">${f.rpm} RPM (${pct}%)</span>
          `;
          fList.appendChild(row);
        }
      });
      ensureFanSpinLoop();
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
    applySvgZoneVisualStyle(z, col);
  });
}

function syncInspectorFromView() {
  const activeProf = state.profiles.find((p) => p.active) || state.profiles[0];
  const viewDev = getTargetDeviceForView(state.activeView);
  if (!activeProf || !activeProf.lightsets || !viewDev) return;

  const pwrSet = new Set(findPowerButtonLights().map((p) => `${p.pid}:${p.lid}`));
  let chosenLs = null;
  for (const ls of activeProf.lightsets) {
    const lights = ls.lights || [];
    if (lights.some((gl) => gl.did === viewDev.pid && !pwrSet.has(`${gl.did}:${gl.lid}`))) {
      chosenLs = ls;
      break;
    }
  }
  if (!chosenLs) {
    chosenLs = activeProf.lightsets.find((ls) => (ls.lights || []).some((gl) => gl.did === viewDev.pid));
  }
  if (!chosenLs) return;

  const col1 = chosenLs.color || '#00f0ff';
  const col2 = chosenLs.color2 || state.colorSecondary || '#7b2cbf';
  const eff = chosenLs.effect || 'static';
  const spd = typeof chosenLs.speed === 'number' ? Math.max(1, Math.min(100, chosenLs.speed)) : state.speed;

  state.colorPrimary = col1;
  state.colorSecondary = col2;
  state.effect = eff;
  state.speed = spd;
  if (eff === 'aurora') {
    state.auroraColors = [col1, col2, chosenLs.color3 || col1];
  }

  document.getElementById('color-primary').value = col1;
  document.getElementById('hex-primary').value = col1.toUpperCase();
  document.getElementById('color-secondary').value = col2;
  document.getElementById('effect-speed').value = spd;
  syncLightingControlsForView();
}

function syncVirtualLightingFromState() {
  const activeProf = state.profiles.find((p) => p.active) || state.profiles[0];
  const lightsOn = state.settings.lightsOn !== false;

  // Build exact (pid, lid) -> color & effect lookup from active profile lightsets
  const litColorMap = new Map();
  const litEffectMap = new Map();
  const deviceFallbackColor = new Map();
  const powerKeySet = new Set(findPowerButtonLights().map((p) => `${p.pid}:${p.lid}`));

  if (lightsOn && activeProf && Array.isArray(activeProf.lightsets)) {
    activeProf.lightsets.forEach((ls) => {
      const col = ls.color || '#00f0ff';
      const eff = ls.effect || 'static';
      (ls.lights || []).forEach((gl) => {
        const key = `${gl.did}:${gl.lid}`;
        litColorMap.set(key, col);
        litEffectMap.set(key, eff);
        if (!powerKeySet.has(key) && !deviceFallbackColor.has(gl.did)) {
          deviceFallbackColor.set(gl.did, col);
        }
      });
    });
  }

  // 1. Sync Keyboard & Numpad keys accurately against keyboard device
  const kbdDev = getTargetDeviceForView('keyboard');
  const kbdPerKey = isPerKeyKeyboard(kbdDev);
  const kbdFallback = kbdDev ? deviceFallbackColor.get(kbdDev.pid) : null;
  const pwrLights = findPowerButtonLights();

  document.querySelectorAll('.keycap[data-id]').forEach((keyEl) => {
    const keyId = keyEl.dataset.id;
    let col = null;
    let eff = '';
    if (keyId === 'power') {
      for (const pl of pwrLights) {
        const hit = litColorMap.get(`${pl.pid}:${pl.lid}`);
        if (hit) { col = hit; eff = litEffectMap.get(`${pl.pid}:${pl.lid}`) || ''; break; }
      }
    } else {
      const keyDef = KEY_BY_ID.get(keyId);
      if (keyDef && kbdDev) {
        const lids = findLightsForKey(keyDef, kbdDev);
        for (const lid of lids) {
          const hit = litColorMap.get(`${kbdDev.pid}:${lid}`);
          if (hit) {
            col = hit;
            eff = litEffectMap.get(`${kbdDev.pid}:${lid}`) || '';
            break;
          }
        }
        if (!col && !kbdPerKey && kbdFallback) {
          col = kbdFallback;
        }
      }
    }
    if (col) {
      state.keyColors[keyId] = col;
      applyKeyVisualStyle(keyEl, col, eff);
    } else {
      delete state.keyColors[keyId];
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
  const mouseSingleZone = mouseDev && (mouseDev.lights || []).filter((l) => (l.flags & 1) === 0).length <= 1;
  document.querySelectorAll('.mouse-zone').forEach((zoneEl) => {
    let col = null;
    if (mouseDev) {
      const lids = findLightsForZone(zoneEl.dataset.zoneId, zoneEl.dataset.zoneName, mouseDev);
      for (const lid of lids) {
        const hit = litColorMap.get(`${mouseDev.pid}:${lid}`);
        if (hit) { col = hit; break; }
      }
      if (!col && mouseSingleZone) col = deviceFallbackColor.get(mouseDev.pid) || null;
    }
    applySvgZoneVisualStyle(zoneEl, col);
  });

  // 4. Sync Chassis zones
  const chassisDev = getTargetDeviceForView('chassis');
  const chassisSingleZone = chassisDev && (chassisDev.lights || []).filter((l) => (l.flags & 1) === 0).length <= 1;
  document.querySelectorAll('.chassis-zone').forEach((zoneEl) => {
    let col = null;
    if (zoneEl.dataset.zoneId === 'power') {
      for (const pl of pwrLights) {
        const hit = litColorMap.get(`${pl.pid}:${pl.lid}`);
        if (hit) { col = hit; break; }
      }
    } else if (chassisDev) {
      const lids = findLightsForZone(zoneEl.dataset.zoneId, zoneEl.dataset.zoneName, chassisDev);
      for (const lid of lids) {
        const hit = litColorMap.get(`${chassisDev.pid}:${lid}`);
        if (hit) { col = hit; break; }
      }
      if (!col && chassisSingleZone) col = deviceFallbackColor.get(chassisDev.pid) || null;
    }
    applySvgZoneVisualStyle(zoneEl, col);
  });
}

function sendProbeLed() {
  const dev = state.devices.find((d) => d.pid === state.selectedDevicePid) || state.devices[0];
  if (!dev) return;
  sendCommand({ cmd: 'probeLed', pid: dev.pid, lightId: state.probeLedId });
}

function updateProbeDeckUI() {
  const deck = document.getElementById('led-probe-deck');
  const toggle = document.getElementById('probe-toggle');
  const stateTxt = document.getElementById('probe-state-text');
  const devSel = document.getElementById('probe-dev-select');
  const codeLbl = document.getElementById('probe-code-label');
  const mapLbl = document.getElementById('probe-mapped-label');
  const prevBtn = document.getElementById('probe-prev');
  const nextBtn = document.getElementById('probe-next');
  const addBtn = document.getElementById('probe-add');
  const remBtn = document.getElementById('probe-remove');
  const tpBtn = document.getElementById('probe-bind-tp');
  const pwrBtn = document.getElementById('probe-bind-pwr');
  if (!deck) return;

  const dev = state.devices.find((d) => d.pid === state.selectedDevicePid) || state.devices[0] || null;
  const existing = dev && dev.lights ? dev.lights.find((l) => l.id === state.probeLedId) : null;

  deck.classList.toggle('probing', state.probeActive);
  if (toggle) toggle.checked = state.probeActive;
  if (stateTxt) stateTxt.textContent = state.probeActive ? 'LED PROBE ACTIVE' : 'LED PROBE INACTIVE';
  if (devSel && dev) devSel.value = String(dev.pid);
  if (codeLbl) codeLbl.textContent = `LED #${state.probeLedId}`;
  if (mapLbl) {
    if (state.probeActive) {
      mapLbl.textContent = existing ? `Mapped: ${existing.name}` : 'Unmapped ID (click + Add LED)';
    } else {
      mapLbl.textContent = existing ? `Mapped: ${existing.name} (Switch ON)` : 'Switch ON to test IDs in red';
    }
  }

  const locked = !state.probeActive || !dev;
  if (prevBtn) prevBtn.disabled = locked;
  if (nextBtn) nextBtn.disabled = locked;
  if (tpBtn) tpBtn.disabled = locked;
  if (pwrBtn) pwrBtn.disabled = locked;
  if (addBtn) addBtn.disabled = locked || !!existing;
  if (remBtn) remBtn.disabled = locked || !existing;
}

function setProbeActive(active) {
  const next = !!active;
  if (state.probeActive === next) {
    updateProbeDeckUI();
    return;
  }
  state.probeActive = next;
  const toggle = document.getElementById('probe-toggle');
  if (toggle) toggle.checked = next;
  if (next) {
    sendProbeLed();
  } else {
    sendCommand({ cmd: 'endProbe' });
  }
  updateProbeDeckUI();
}

function initLedProbeDeck() {
  const toggle = document.getElementById('probe-toggle');
  const devSel = document.getElementById('probe-dev-select');
  const prevBtn = document.getElementById('probe-prev');
  const nextBtn = document.getElementById('probe-next');
  const addBtn = document.getElementById('probe-add');
  const remBtn = document.getElementById('probe-remove');
  const tpBtn = document.getElementById('probe-bind-tp');
  const pwrBtn = document.getElementById('probe-bind-pwr');

  if (toggle) {
    toggle.addEventListener('change', (e) => {
      setProbeActive(e.target.checked);
    });
  }
  if (devSel) {
    devSel.addEventListener('change', (e) => {
      const nextPid = parseInt(e.target.value, 10);
      if (Number.isNaN(nextPid)) return;
      state.selectedDevicePid = nextPid;
      state.selectedHwLights.clear();
      const dd = document.getElementById('hw-device-dropdown');
      if (dd) dd.value = String(nextPid);
      renderHardwareLights();
      refreshSelectionUI();
      updateProbeDeckUI();
      syncVirtualLightingFromState();
      if (state.probeActive) sendProbeLed();
    });
  }
  if (prevBtn) {
    prevBtn.addEventListener('click', () => {
      state.probeLedId = (state.probeLedId + 255) % 256;
      updateProbeDeckUI();
      if (state.probeActive) sendProbeLed();
    });
  }
  if (nextBtn) {
    nextBtn.addEventListener('click', () => {
      state.probeLedId = (state.probeLedId + 1) % 256;
      updateProbeDeckUI();
      if (state.probeActive) sendProbeLed();
    });
  }
  if (addBtn) {
    addBtn.addEventListener('click', () => {
      const dev = state.devices.find((d) => d.pid === state.selectedDevicePid) || state.devices[0];
      if (!dev) return;
      sendCommand({ cmd: 'addLight', pid: dev.pid, lightId: state.probeLedId, name: `LED Zone ${state.probeLedId}` });
    });
  }
  if (remBtn) {
    remBtn.addEventListener('click', () => {
      const dev = state.devices.find((d) => d.pid === state.selectedDevicePid) || state.devices[0];
      if (!dev) return;
      state.selectedHwLights.delete(state.probeLedId);
      sendCommand({ cmd: 'removeLight', pid: dev.pid, lightId: state.probeLedId });
    });
  }
  if (tpBtn) {
    tpBtn.addEventListener('click', () => {
      const dev = state.devices.find((d) => d.pid === state.selectedDevicePid) || state.devices[0];
      if (!dev) return;
      sendCommand({ cmd: 'bindLightMapping', pid: dev.pid, lightId: state.probeLedId, name: 'RGB Touchpad', flags: 0 });
    });
  }
  if (pwrBtn) {
    pwrBtn.addEventListener('click', () => {
      const dev = state.devices.find((d) => d.pid === state.selectedDevicePid) || state.devices[0];
      if (!dev) return;
      sendCommand({ cmd: 'bindLightMapping', pid: dev.pid, lightId: state.probeLedId, name: 'Power Button', flags: 1 });
    });
  }
  updateProbeDeckUI();
}

function applyFullState(data) {
  state.devices = data.devices || [];
  state.profiles = data.profiles || [];
  state.activeProfileId = data.activeProfileId || 0;
  const activePreset = data.activePreset || '';

  if (data.settings) {
    state.settings = data.settings;
    updateLightsButton();
    document.getElementById('master-brightness').value = data.settings.brightness ?? 255;
    document.getElementById('master-brightness-val').textContent = `${Math.round(((data.settings.brightness ?? 255) / 255) * 100)}%`;
    document.getElementById('set-start-windows').checked = !!data.settings.startWindows;
    document.getElementById('set-start-minimized').checked = !!data.settings.startMinimized;
    const setAlwaysAdmin = document.getElementById('set-always-admin');
    if (setAlwaysAdmin) setAlwaysAdmin.checked = !!data.settings.alwaysAdmin;
    const setStartupIntro = document.getElementById('set-startup-intro');
    if (setStartupIntro) setStartupIntro.checked = data.settings.startupIntro !== false;
    document.getElementById('set-prof-switch').checked = !!data.settings.enableProfSwitch;
    document.getElementById('set-gamma').checked = !!data.settings.gammaCorrection;
    document.getElementById('set-fan-control').checked = !!data.settings.fanControl;
    document.getElementById('set-off-battery').checked = !!data.settings.offOnBattery;
    document.getElementById('set-dim-battery').checked = !!data.settings.dimmedBatt;
  }

  if (Array.isArray(data.fanCurves) && data.fanCurves.length > 0 && Array.isArray(data.fanCurves[0].points) && data.fanCurves[0].points.length > 0) {
    state.fanCurve = data.fanCurves[0].points.map((pt) => ({ temp: pt.temp, boost: pt.boost }));
    renderFanCurve();
  }
  updateCurvePresetBadge();

  const dd = document.getElementById('hw-device-dropdown');
  const pdd = document.getElementById('probe-dev-select');
  const hasSelected = state.devices.some((d) => d.pid === state.selectedDevicePid);
  if (!hasSelected) {
    const viewDev = getTargetDeviceForView(state.activeView);
    state.selectedDevicePid = viewDev ? viewDev.pid : (state.devices.length > 0 ? state.devices[0].pid : 0);
  }
  dd.innerHTML = '';
  if (pdd) pdd.innerHTML = '';
  state.devices.forEach((d) => {
    const opt = document.createElement('option');
    opt.value = d.pid;
    opt.textContent = d.name;
    dd.appendChild(opt);
    if (pdd) {
      const popt = document.createElement('option');
      popt.value = d.pid;
      popt.textContent = d.name;
      pdd.appendChild(popt);
    }
  });
  if (state.selectedDevicePid) {
    dd.value = String(state.selectedDevicePid);
    if (pdd) pdd.value = String(state.selectedDevicePid);
  }
  renderHardwareLights();
  updateProbeDeckUI();
  syncVirtualLightingFromState();
  updateUnmappedHelper();

  const presetBanner = document.getElementById('active-preset-banner');
  const presetNameEl = document.getElementById('active-preset-name');
  const btnResetPreset = document.getElementById('btn-reset-preset');
  if (presetBanner) presetBanner.style.display = activePreset ? 'block' : 'none';
  if (btnResetPreset) btnResetPreset.style.display = activePreset ? 'inline-flex' : 'none';
  if (presetNameEl) presetNameEl.textContent = activePreset || '—';

  document.querySelectorAll('#game-presets .preset-chip').forEach((chip) => {
    chip.classList.toggle('active', !!activePreset && chip.textContent.trim() === activePreset);
  });

  const pGrid = document.getElementById('profiles-grid');
  pGrid.innerHTML = '';
  state.profiles.forEach((p) => {
    if (p.isPreset) return;
    if (p.active && !activePreset) document.getElementById('hud-profile').textContent = p.name;
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
  if (activePreset) {
    document.getElementById('hud-profile').textContent = `Preset: ${activePreset}`;
  }
  syncLightingControlsForView();
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
    row.innerHTML = `
      <span>#${l.id} ${l.name || 'Unnamed LED'}</span>
      <div class="hw-light-actions">
        <button class="hw-blink-btn" title="Blink physical LED">Blink</button>
        <button class="hw-remove-btn" title="Remove LED mapping">&#10005;</button>
      </div>
    `;
    row.addEventListener('click', (e) => {
      if (e.target.closest('.hw-blink-btn, .hw-remove-btn')) return;
      state.selectedTargets.clear();
      if (!e.ctrlKey && !e.shiftKey) state.selectedHwLights.clear();
      if (state.selectedHwLights.has(l.id)) state.selectedHwLights.delete(l.id);
      else state.selectedHwLights.add(l.id);
      refreshSelectionUI();
    });
    row.querySelector('.hw-blink-btn').addEventListener('click', () => {
      sendCommand({ cmd: 'blinkLight', pid: dev.pid, lightId: l.id });
    });
    row.querySelector('.hw-remove-btn').addEventListener('click', (e) => {
      e.stopPropagation();
      state.selectedHwLights.delete(l.id);
      sendCommand({ cmd: 'removeLight', pid: dev.pid, lightId: l.id });
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
    const pdd = document.getElementById('probe-dev-select');
    if (pdd) pdd.value = String(state.selectedDevicePid);
    state.selectedHwLights.clear();
    renderHardwareLights();
    refreshSelectionUI();
    updateProbeDeckUI();
    syncVirtualLightingFromState();
    if (state.probeActive) sendProbeLed();
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

  const bindSelectedHwLight = (name, flags) => {
    const dev = state.devices.find((d) => d.pid === state.selectedDevicePid) || state.devices[0];
    const hint = document.getElementById('hw-mapper-hint');
    if (!dev || state.selectedHwLights.size === 0) {
      if (hint) hint.textContent = 'First click a LED row below to highlight it, then click Bind.';
      return;
    }
    const lid = Array.from(state.selectedHwLights)[0];
    sendCommand({ cmd: 'bindLightMapping', pid: dev.pid, lightId: lid, name, flags });
    if (hint) hint.textContent = `Bound LED #${lid} on ${dev.name} as ${name}.`;
  };

  const btnBindTouchpad = document.getElementById('btn-bind-touchpad');
  if (btnBindTouchpad) {
    btnBindTouchpad.addEventListener('click', () => bindSelectedHwLight('RGB Touchpad', 0));
  }
  const btnBindPower = document.getElementById('btn-bind-power');
  if (btnBindPower) {
    btnBindPower.addEventListener('click', () => bindSelectedHwLight('Power Button', 1));
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

  document.querySelectorAll('[data-tpreset]').forEach((btn) => {
    btn.addEventListener('click', () => {
      const preset = THERMAL_PRESETS[btn.dataset.tpreset];
      if (!preset) return;
      state.fanCurve = preset.points.map((p) => ({ ...p }));
      renderFanCurve();
      updateCurvePresetBadge();
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
      syncLightingControlsForView();

      sendCommand({
        cmd: 'applyGamePreset',
        name: p.name,
        color: p.color,
        color2: p.color2,
        color3: p.color3,
        effect: p.effect,
        speed: p.speed
      });
    });
  });

  const btnResetPreset = document.getElementById('btn-reset-preset');
  if (btnResetPreset) {
    btnResetPreset.addEventListener('click', () => {
      sendCommand({ cmd: 'restorePreset' });
    });
  }

  document.getElementById('btn-save-curve').addEventListener('click', () => {
    sendCommand({ cmd: 'setFanCurve', fanId: 0, sensorId: 0, active: true, points: state.fanCurve });
  });

  document.getElementById('btn-refresh-batt').addEventListener('click', () => {
    sendCommand({ cmd: 'getTelemetry' });
  });

  const btnTestIntro = document.getElementById('btn-test-intro');
  if (btnTestIntro) {
    btnTestIntro.addEventListener('click', () => {
      sendCommand({ cmd: 'testIntro' });
    });
  }

  const settingMap = {
    'set-start-windows': 'startWindows',
    'set-start-minimized': 'startMinimized',
    'set-always-admin': 'alwaysAdmin',
    'set-startup-intro': 'startupIntro',
    'set-prof-switch': 'enableProfSwitch',
    'set-gamma': 'gammaCorrection',
    'set-fan-control': 'fanControl',
    'set-off-battery': 'offOnBattery',
    'set-dim-battery': 'dimmedBatt'
  };

  Object.entries(settingMap).forEach(([elId, key]) => {
    const el = document.getElementById(elId);
    if (!el) return;
    el.addEventListener('change', (e) => {
      sendCommand({ cmd: 'setSettings', [key]: e.target.checked });
      if (key === 'alwaysAdmin' && e.target.checked && !state.settings.isAdmin) {
        setTimeout(() => window.restartAsAdmin(), 180);
      }
    });
  });
}

document.addEventListener('DOMContentLoaded', () => {
  buildKeyboard();
  buildNumpad();
  initNavigation();
  initLightingControls();
  initLedProbeDeck();
  initWindowControls();
  initMiscActions();
  renderFanCurve();
  syncLightingControlsForView();

  if (hasWebView) {
    sendCommand({ cmd: 'getState' });
    sendCommand({ cmd: 'getTelemetry' });
  } else {
    applyFullState({
      devices: [{ pid: 0x550, name: 'Alienware Integrated Keyboard (Preview)', lights: [{ id: 1, name: 'WASD Cluster' }, { id: 2, name: 'Logo Emblem' }] }],
      profiles: [{ id: 0, name: 'Default', isDefault: true, active: true }, { id: 1, name: 'Orbital Gaming', isDefault: false, active: false }],
      settings: { lightsOn: true, brightness: 255, startWindows: true, startMinimized: true, startupIntro: true }
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
