// AlienCockpit UI Controller
const hasWebView = !!(window.chrome && window.chrome.webview);

const REAR_EFFECTS = new Set(['scanner', 'comet', 'converge', 'syncbreath', 'syncmorph', 'aurora_bar']);
const STD_TO_REAR_EFFECT = {
  breathing: 'syncbreath',
  morph: 'syncmorph',
  aurora: 'aurora_bar',
  wave: 'scanner',
  pulse: 'converge',
  spectrum: 'comet'
};
const REAR_TO_STD_EFFECT = {
  syncbreath: 'breathing',
  syncmorph: 'morph',
  aurora_bar: 'aurora',
  scanner: 'wave',
  converge: 'pulse',
  comet: 'spectrum'
};

const state = {
  devices: [],
  profiles: [],
  activeProfileId: 0,
  settings: { lightsOn: true, brightness: 255, startWindows: false, startMinimized: true, startupIntro: true, introStyle: 0, wavesMode: false, favorites: [] },
  wavesMode: false,
  favorites: [],
  lastSensors: [],
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
  hasPendingApply: false,
  probeActive: false,
  probeLedId: 0,
  keyColors: {},
  keyBaseColors: {},
  keyBrightness: {},
  keyMeta: {},
  zoneColors: {},
  zoneBaseColors: {},
  zoneBrightnessMap: {},
  zoneMeta: {},
  hwLightMeta: new Map(),
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
    } else if (Array.isArray(data.controllers) || data.wmi) {
      if (typeof renderHardwareDiagnostics === 'function') {
        renderHardwareDiagnostics(data);
      }
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
  if (dev.vid === 0x187c || /elc|chassis|tron|lightbar/i.test(dev.name || '')) return false;
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
  if (view === 'chassis') {
    const aw0551 = state.devices.find((d) => d.present && d.pid === 0x0551) ||
      state.devices.find((d) => d.pid === 0x0551) ||
      state.devices.find((d) => d.present && d.version === 4) ||
      state.devices.find((d) => d.version === 4);
    if (aw0551 && (!cur || !isChassisDevice(cur) || cur.pid !== aw0551.pid)) return aw0551;
  }
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
  const dev0551 = state.devices.find((d) => d.pid === 0x0551 && (d.lights || []).some((l) => l.id === 31));
  if (dev0551) {
    return [{ pid: dev0551.pid, lid: 31 }];
  }
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

  if (!Number.isNaN(numZid)) {
    const exact = dev.lights.find((l) => l.id === numZid);
    if (exact) return [exact.id];
  }

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

  if (!Number.isNaN(numZid) && numZid >= 10 && usable.length > 0) {
    return [usable[(numZid - 10) % usable.length].id];
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

function normalizeHex(hex) {
  if (typeof hex !== 'string') return null;
  const trimmed = hex.trim().toLowerCase();
  return /^#[0-9a-f]{6}$/.test(trimmed) ? trimmed : null;
}

function setPrimaryColorUI(hex) {
  const norm = normalizeHex(hex);
  if (!norm) return;
  state.colorPrimary = norm;
  const cPrimary = document.getElementById('color-primary');
  const hPrimary = document.getElementById('hex-primary');
  if (cPrimary) cPrimary.value = norm;
  if (hPrimary) hPrimary.value = norm.toUpperCase();
}

function setZoneBrightnessUI(pct) {
  const val = Math.max(5, Math.min(100, Math.round(Number(pct) || 100)));
  state.zoneBrightness = val;
  const zBright = document.getElementById('zone-brightness');
  const zBrightVal = document.getElementById('zone-brightness-val');
  if (zBright) zBright.value = String(val);
  if (zBrightVal) zBrightVal.textContent = `${val}%`;
}

function getZoneMetaKey(view, zoneId) {
  const idStr = String(zoneId);
  if (idStr === 'touchpad' || idStr === 'power') return idStr;
  return `${view}:${idStr}`;
}

function markApplyPending(pending) {
  state.hasPendingApply = !!pending;
  const btn = document.getElementById('btn-apply-lighting');
  if (btn) btn.classList.toggle('has-pending-apply', state.hasPendingApply);
}

function markDirtyIfSelected() {
  if (state.selectedTargets.size > 0 || state.selectedHwLights.size > 0) {
    markApplyPending(true);
  }
}

function isRearLightbarSelected() {
  if (state.activeView === 'mouse') return false;
  if (state.selectedHwLights.size > 0) {
    const dev = state.devices.find((d) => d.pid === state.selectedDevicePid);
    if (dev && isChassisDevice(dev)) {
      const ids = Array.from(state.selectedHwLights);
      return ids.some((lid) => lid >= 0 && lid <= 30) && ids.every((lid) => lid >= 0 && lid <= 30);
    }
    return false;
  }
  if (state.activeView !== 'chassis') return false;
  if (state.selectedTargets.size > 0) {
    return Array.from(state.selectedTargets).some((id) => /^\d+$/.test(String(id)) && Number(id) >= 0 && Number(id) <= 30);
  }
  return REAR_EFFECTS.has(state.effect);
}

function syncAuroraPaletteActive(c1, c2, c3) {
  const target = [normalizeHex(c1), normalizeHex(c2), normalizeHex(c3 || c1)];
  let matched = false;
  document.querySelectorAll('.palette-btn').forEach((btn) => {
    const cols = (btn.dataset.colors || '').split(',').map((s) => normalizeHex(s)).filter(Boolean);
    const isHit = cols.length >= 2 &&
      cols[0] === target[0] &&
      cols[1] === target[1] &&
      (cols[2] || cols[0]) === target[2];
    if (isHit) matched = true;
    btn.classList.toggle('active', isHit);
  });
  return matched;
}

function applyMetaToInspector(meta) {
  if (!meta) return;
  if (meta.color) setPrimaryColorUI(meta.color);
  if (typeof meta.brightness === 'number') setZoneBrightnessUI(meta.brightness);

  const isMouse = state.activeView === 'mouse';
  const isRear = !isMouse && isRearLightbarSelected();
  let eff = meta.effect || 'static';
  if (eff === 'summer' || eff === 'waves') eff = 'static';
  if (isRear && STD_TO_REAR_EFFECT[eff]) eff = STD_TO_REAR_EFFECT[eff];
  if (!isRear && REAR_TO_STD_EFFECT[eff]) eff = REAR_TO_STD_EFFECT[eff];
  if (isMouse && eff !== 'static' && eff !== 'morph') eff = 'static';
  state.effect = eff;

  if (typeof meta.speed === 'number') {
    const spd = Math.max(1, Math.min(100, Math.round(meta.speed)));
    state.speed = spd;
    const spdEl = document.getElementById('effect-speed');
    if (spdEl) spdEl.value = String(spd);
  }

  const col2 = normalizeHex(meta.color2);
  if (col2) {
    state.colorSecondary = col2;
    const c2El = document.getElementById('color-secondary');
    if (c2El) c2El.value = col2;
  }

  if (eff === 'aurora' || eff === 'aurora_bar') {
    const c1 = normalizeHex(meta.color) || state.colorPrimary;
    const c2Use = col2 || state.colorSecondary;
    const c3Use = normalizeHex(meta.color3) || c1;
    state.auroraColors = [c1, c2Use, c3Use];
    syncAuroraPaletteActive(c1, c2Use, c3Use);
  }

  syncLightingControlsForView();
  markApplyPending(false);
}

function loadSelectionColorAndBrightness() {
  if (state.selectedHwLights.size > 0) {
    for (const lid of state.selectedHwLights) {
      const meta = state.hwLightMeta.get(lid);
      if (meta && meta.color) {
        applyMetaToInspector(meta);
        return;
      }
    }
    syncLightingControlsForView();
    return;
  }

  if (state.selectedTargets.size === 0) {
    syncLightingControlsForView();
    return;
  }

  for (const t of state.selectedTargets) {
    if (t === 'touchpad' || t === 'power') {
      const meta = state.zoneMeta[t] || state.keyMeta[t];
      if (meta && meta.color) {
        applyMetaToInspector(meta);
        return;
      }
      const col = state.zoneBaseColors[t] || state.keyBaseColors[t];
      if (col) {
        applyMetaToInspector({ color: col, brightness: state.zoneBrightnessMap[t] ?? state.keyBrightness[t] ?? 100 });
        return;
      }
    } else if (state.activeView === 'keyboard' && KEY_BY_ID.has(t)) {
      const meta = state.keyMeta[t];
      if (meta && meta.color) {
        applyMetaToInspector(meta);
        return;
      }
      const col = state.keyBaseColors[t];
      if (col) {
        applyMetaToInspector({ color: col, brightness: state.keyBrightness[t] ?? 100 });
        return;
      }
    } else {
      const zKey = getZoneMetaKey(state.activeView, t);
      const meta = state.zoneMeta[zKey];
      if (meta && meta.color) {
        applyMetaToInspector(meta);
        return;
      }
      const col = state.zoneBaseColors[zKey];
      if (col) {
        applyMetaToInspector({ color: col, brightness: state.zoneBrightnessMap[zKey] ?? 100 });
        return;
      }
    }
  }
  syncLightingControlsForView();
}

function updateWavesModeUI() {
  const active = !!state.wavesMode;
  const btn = document.getElementById('btn-global-waves');
  const txt = document.getElementById('global-waves-state');
  if (btn) {
    btn.classList.toggle('active', active);
    if (typeof btn.setAttribute === 'function') {
      btn.setAttribute('aria-pressed', active ? 'true' : 'false');
    }
  }
  if (txt) {
    txt.textContent = active ? 'ON' : 'OFF';
  }
  const litAndWaves = active && state.settings.lightsOn !== false;
  document.querySelectorAll('.device-canvas').forEach((canvas) => {
    canvas.classList.toggle('waves-active', litAndWaves);
  });
}

function loadFavoritesFallback() {
  try {
    if (typeof localStorage !== 'undefined' && localStorage.getItem) {
      const raw = JSON.parse(localStorage.getItem('aliencockpit_favorites') || '[]');
      if (Array.isArray(raw)) {
        return Array.from(new Set(raw.map(normalizeHex).filter(Boolean)));
      }
    }
  } catch (_) {}
  return [];
}

function saveFavorites(favs) {
  const unique = Array.from(new Set((favs || []).map(normalizeHex).filter(Boolean))).slice(0, 24);
  state.favorites = unique;
  state.settings.favorites = [...unique];
  try {
    if (typeof localStorage !== 'undefined' && localStorage.setItem) {
      localStorage.setItem('aliencockpit_favorites', JSON.stringify(unique));
    }
  } catch (_) {}
  sendCommand({ cmd: 'setSettings', favorites: unique });
}

function renderFavoriteColors() {
  const container = document.getElementById('fav-color-swatches');
  if (!container) return;
  container.innerHTML = '';
  if (!state.favorites || state.favorites.length === 0) {
    const empty = document.createElement('span');
    empty.className = 'hint-text';
    empty.textContent = 'No favorite colors yet';
    container.appendChild(empty);
    return;
  }
  state.favorites.forEach((hex) => {
    const sw = document.createElement('button');
    sw.type = 'button';
    sw.className = 'fav-swatch';
    sw.dataset.color = hex;
    sw.style.background = hex;
    sw.title = `${hex.toUpperCase()} — Click to apply, Right-click to remove`;
    sw.addEventListener('click', () => {
      setPrimaryColorUI(hex);
      markDirtyIfSelected();
    });
    sw.addEventListener('contextmenu', (e) => {
      if (e && typeof e.preventDefault === 'function') e.preventDefault();
      const next = state.favorites.filter((c) => c !== hex);
      saveFavorites(next);
      renderFavoriteColors();
    });
    container.appendChild(sw);
  });
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
    if (effect === 'summer' || effect === 'waves') {
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
  if (zoneEl.classList && (zoneEl.classList.contains('chassis-led-seg') || zoneEl.classList.contains('chassis-icon-card'))) {
    const isSeg = zoneEl.classList.contains('chassis-led-seg');
    const fallback = isSeg ? '#1e293b' : '#475569';
    if (zoneEl.style && typeof zoneEl.style.setProperty === 'function') {
      zoneEl.style.setProperty('--zone-color', color || fallback);
    }
    if (isSeg) {
      zoneEl.style.background = color || '#1e293b';
      zoneEl.style.boxShadow = color ? `0 0 10px ${color}` : 'none';
    } else {
      const glyph = zoneEl.querySelector ? zoneEl.querySelector('.chassis-icon-glyph, .chassis-power-ring') : null;
      if (glyph && glyph.style) {
        glyph.style.filter = color ? `drop-shadow(0 0 10px ${color})` : 'none';
      }
    }
    return;
  }
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
        if (state.wavesMode) {
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
      if (state.wavesMode && key.id !== 'power') {
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
  const added = !state.selectedTargets.has(key);
  if (added) state.selectedTargets.add(key);
  else state.selectedTargets.delete(key);
  refreshSelectionUI();
  if (added) {
    if (state.selectedTargets.size === 1) {
      loadSelectionColorAndBrightness();
    } else {
      syncLightingControlsForView();
    }
  } else if (state.selectedTargets.size > 0) {
    loadSelectionColorAndBrightness();
  } else {
    markApplyPending(false);
    syncLightingControlsForView();
  }
}

function formatTargetLabel(t) {
  if (t === 'touchpad') return 'RGB Touchpad';
  if (t === 'power') return 'Power Button';
  if (state.activeView === 'keyboard') {
    const kd = KEY_BY_ID.get(t);
    if (kd) return kd.name || kd.label;
  }
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
      markApplyPending(false);
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
      if (state.selectedTargets.size > 0) {
        loadSelectionColorAndBrightness();
      } else {
        markApplyPending(false);
        syncLightingControlsForView();
      }
    });
  });

  // Mouse, Chassis & Touchpad zones
  let chassisDragMoved = false;
  let isChassisDragging = false;
  window.addEventListener('mouseup', () => { isChassisDragging = false; });

  document.querySelectorAll('.mouse-zone, .chassis-zone, .touchpad-zone').forEach((zone) => {
    if (zone.classList && zone.classList.contains('chassis-led-seg')) {
      zone.addEventListener('mousedown', (e) => {
        if (e.button !== 0) return;
        isChassisDragging = true;
        chassisDragMoved = false;
        state.selectedHwLights.clear();
        if (!e.ctrlKey && !e.shiftKey) state.selectedTargets.clear();
        state.selectedTargets.add(zone.dataset.zoneId);
        refreshSelectionUI();
        if (state.selectedTargets.size === 1) {
          loadSelectionColorAndBrightness();
        } else {
          syncLightingControlsForView();
        }
      });
      zone.addEventListener('mouseenter', (e) => {
        if (!isChassisDragging || !(e.buttons & 1)) return;
        chassisDragMoved = true;
        state.selectedTargets.add(zone.dataset.zoneId);
        refreshSelectionUI();
        const lbl = document.getElementById('chassis-zone-label');
        if (lbl) lbl.textContent = `Selected ${state.selectedTargets.size} chassis LEDs`;
      });
    }
    zone.addEventListener('click', (e) => {
      if (chassisDragMoved && zone.classList && zone.classList.contains('chassis-led-seg')) {
        chassisDragMoved = false;
        loadSelectionColorAndBrightness();
        return;
      }
      chassisDragMoved = false;
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

  const bindChassisQuick = (id, selector, labelText) => {
    const btn = document.getElementById(id);
    if (!btn) return;
    btn.addEventListener('click', () => {
      state.selectedHwLights.clear();
      state.selectedTargets.clear();
      if (selector) {
        document.querySelectorAll(selector).forEach((el) => {
          if (el.dataset && el.dataset.zoneId) state.selectedTargets.add(el.dataset.zoneId);
        });
      }
      refreshSelectionUI();
      const lbl = document.getElementById('chassis-zone-label');
      if (lbl) lbl.textContent = labelText;
      if (state.selectedTargets.size > 0) {
        loadSelectionColorAndBrightness();
      } else {
        markApplyPending(false);
        syncLightingControlsForView();
      }
    });
  };
  bindChassisQuick('chassis-sel-rear', '#rear-lightbar-strip .chassis-zone', 'Selected: Rear Lightbar (#0 – #30)');
  bindChassisQuick('chassis-sel-touchpad', '#touchpad-strip .chassis-zone', 'Selected: RGB Touchpad (#35 – #44)');
  bindChassisQuick('chassis-sel-all', '#view-chassis .chassis-zone', 'Selected: All AA18250 Chassis Zones (45 LEDs)');
  bindChassisQuick('chassis-sel-clear', null, 'Click any chassis zone (including Rear, Fans, Lid, Power Button, or Touchpad) to customize');
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
  const baseHex = normalizeHex(colorHex) || colorHex;
  const pwrMeta = {
    color: baseHex,
    brightness: state.zoneBrightness,
    effect: 'static',
    speed: 50,
    color2: baseHex,
    color3: baseHex
  };
  state.keyBaseColors.power = baseHex;
  state.keyBrightness.power = state.zoneBrightness;
  state.keyMeta.power = { ...pwrMeta };
  state.zoneBaseColors.power = baseHex;
  state.zoneBrightnessMap.power = state.zoneBrightness;
  state.zoneMeta.power = { ...pwrMeta };
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
  const isRear = !isMouse && isRearLightbarSelected();

  if (!isMouse) {
    if (isRear && STD_TO_REAR_EFFECT[state.effect]) {
      state.effect = STD_TO_REAR_EFFECT[state.effect];
    } else if (!isRear && REAR_TO_STD_EFFECT[state.effect]) {
      state.effect = REAR_TO_STD_EFFECT[state.effect];
    }
  } else if (state.effect !== 'static' && state.effect !== 'morph') {
    state.effect = 'static';
  }

  document.querySelectorAll('#effect-grid .eff-btn').forEach((b) => {
    const eff = b.dataset.eff;
    let allowed = false;
    if (eff === 'static') {
      allowed = true;
    } else if (isMouse) {
      allowed = eff === 'morph';
    } else if (isRear) {
      allowed = b.classList.contains('eff-rear');
    } else {
      allowed = b.classList.contains('eff-std');
    }
    b.style.display = allowed ? '' : 'none';
  });

  const activeBtn = document.querySelector(`#effect-grid .eff-btn[data-eff="${state.effect}"]`);
  if (!activeBtn || activeBtn.style.display === 'none') {
    state.effect = 'static';
  }

  document.querySelectorAll('#effect-grid .eff-btn').forEach((b) => {
    b.classList.toggle('active', b.dataset.eff === state.effect && b.style.display !== 'none');
  });

  const showSec = isMouse || ['morph', 'aurora', 'breathing', 'syncmorph', 'syncbreath', 'aurora_bar'].includes(state.effect);
  const secWrap = document.getElementById('secondary-color-wrap');
  if (secWrap) secWrap.style.display = showSec ? 'flex' : 'none';
  const auroraWrap = document.getElementById('aurora-palette-wrap');
  if (auroraWrap) auroraWrap.style.display = (!isMouse && (state.effect === 'aurora' || state.effect === 'aurora_bar')) ? 'flex' : 'none';
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
    markDirtyIfSelected();
  });

  hPrimary.addEventListener('change', () => {
    if (/^#[0-9a-fA-F]{6}$/.test(hPrimary.value)) {
      state.colorPrimary = hPrimary.value.toLowerCase();
      cPrimary.value = hPrimary.value.toLowerCase();
      markDirtyIfSelected();
    }
  });

  cSecondary.addEventListener('input', () => {
    state.colorSecondary = cSecondary.value;
    markDirtyIfSelected();
  });

  if (zBright && zBrightVal) {
    zBright.addEventListener('input', () => {
      state.zoneBrightness = Math.max(5, Math.min(100, parseInt(zBright.value, 10) || 100));
      zBrightVal.textContent = `${state.zoneBrightness}%`;
      markDirtyIfSelected();
    });
  }

  document.querySelectorAll('.swatch').forEach((sw) => {
    sw.addEventListener('click', () => {
      setPrimaryColorUI(sw.dataset.color);
      markDirtyIfSelected();
    });
  });

  const btnAddFav = document.getElementById('btn-add-fav-color');
  if (btnAddFav) {
    btnAddFav.addEventListener('click', () => {
      const norm = normalizeHex(state.colorPrimary) || normalizeHex(cPrimary.value);
      if (!norm) return;
      const next = state.favorites.includes(norm)
        ? state.favorites
        : [...state.favorites, norm];
      saveFavorites(next);
      renderFavoriteColors();
    });
  }

  const wavesColorEl = document.getElementById('waves-color-picker');
  const wavesBrightEl = document.getElementById('waves-brightness');
  const wavesBrightValEl = document.getElementById('waves-brightness-val');

  if (wavesColorEl) {
    wavesColorEl.addEventListener('change', (e) => {
      state.settings.wavesColor = e.target.value;
      sendCommand({ cmd: 'setWavesOptions', color: e.target.value });
    });
  }
  if (wavesBrightEl) {
    wavesBrightEl.addEventListener('input', (e) => {
      if (wavesBrightValEl) wavesBrightValEl.textContent = `${e.target.value}%`;
    });
    wavesBrightEl.addEventListener('change', (e) => {
      const b = parseInt(e.target.value, 10) || 100;
      state.settings.wavesBrightness = b;
      sendCommand({ cmd: 'setWavesOptions', brightness: b });
    });
  }

  const btnGlobalWaves = document.getElementById('btn-global-waves');
  if (btnGlobalWaves) {
    btnGlobalWaves.addEventListener('click', () => {
      state.wavesMode = !state.wavesMode;
      state.settings.wavesMode = state.wavesMode;
      updateWavesModeUI();
      syncVirtualLightingFromState();
      sendCommand({
        cmd: 'toggleWaves',
        enabled: state.wavesMode,
        color: wavesColorEl ? wavesColorEl.value : undefined,
        brightness: wavesBrightEl ? (parseInt(wavesBrightEl.value, 10) || 100) : undefined
      });
    });
  }

  document.querySelectorAll('.eff-btn').forEach((btn) => {
    btn.addEventListener('click', () => {
      if (btn.style.display === 'none') return;
      if (state.activeView === 'mouse' && btn.dataset.eff !== 'static' && btn.dataset.eff !== 'morph') {
        return;
      }
      state.effect = btn.dataset.eff;
      syncLightingControlsForView();
      markDirtyIfSelected();
    });
  });

  document.querySelectorAll('.palette-btn').forEach((btn) => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('.palette-btn').forEach((b) => b.classList.remove('active'));
      btn.classList.add('active');
      const cols = (btn.dataset.colors || '').split(',').map((s) => s.trim()).filter(Boolean);
      if (cols.length >= 2) {
        state.auroraColors = [cols[0], cols[1], cols[2] || cols[0]];
        setPrimaryColorUI(cols[0]);
        state.colorSecondary = cols[1];
        document.getElementById('color-secondary').value = cols[1];
        markDirtyIfSelected();
      }
    });
  });

  document.getElementById('effect-speed').addEventListener('input', (e) => {
    state.speed = parseInt(e.target.value, 10);
    markDirtyIfSelected();
  });

  document.getElementById('btn-apply-lighting').addEventListener('click', () => {
    const hwTargets = Array.from(state.selectedHwLights);
    const uiTargets = Array.from(state.selectedTargets);
    if (uiTargets.length === 0 && hwTargets.length === 0) return;

    if (state.probeActive) {
      state.probeActive = false;
      state.probeLedId = 0;
      const toggle = document.getElementById('probe-toggle');
      if (toggle) toggle.checked = false;
      updateProbeDeckUI();
    }

    const baseColor = normalizeHex(state.colorPrimary) || state.colorPrimary;
    const color2Norm = normalizeHex(state.colorSecondary) || state.colorSecondary;
    const color3Norm = normalizeHex(state.auroraColors[2]) || baseColor;
    const commonPayload = {
      color: baseColor,
      color2: color2Norm,
      color3: color3Norm,
      effect: state.effect,
      speed: state.speed,
      zoneBrightness: state.zoneBrightness
    };
    const metaObj = {
      color: baseColor,
      brightness: state.zoneBrightness,
      effect: state.effect,
      speed: state.speed,
      color2: color2Norm,
      color3: color3Norm
    };
    const visColor = scaleHexColor(baseColor, state.zoneBrightness);

    // Direct Hardware LED Mapper selection always goes to currently chosen dropdown device
    if (hwTargets.length > 0) {
      const dev = state.devices.find((d) => d.pid === state.selectedDevicePid) || state.devices[0];
      if (!dev) return;
      hwTargets.forEach((lid) => {
        state.hwLightMeta.set(lid, { ...metaObj });
      });
      sendCommand({
        cmd: 'applyZoneColor',
        pid: dev.pid,
        lightIds: hwTargets,
        zoneName: `LEDs ${hwTargets.slice(0, 4).map((x) => `#${x}`).join(',')}`,
        ...commonPayload
      });
      markApplyPending(false);
      return;
    }

    const hasPower = uiTargets.includes('power');
    let appliedPower = false;
    if (hasPower) {
      appliedPower = sendPowerButtonColor(baseColor);
    }

    if (state.activeView === 'keyboard') {
      const hasTouchpad = uiTargets.includes('touchpad');
      const keyTargets = uiTargets.filter((id) => id !== 'touchpad' && id !== 'power');
      let appliedTouchpad = false;

      if (hasTouchpad) {
        const tpLights = findTouchpadLights();
        if (tpLights.length > 0) {
          state.zoneBaseColors.touchpad = baseColor;
          state.zoneBrightnessMap.touchpad = state.zoneBrightness;
          state.zoneMeta.touchpad = { ...metaObj };
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
          appliedTouchpad = true;
        } else if (keyTargets.length === 0 && !hasPower) {
          document.getElementById('selected-names').textContent = 'No RGB Touchpad LED mapped yet — use Red LED Probe below & click "→ Touchpad"';
          return;
        }
      }

      if (keyTargets.length === 0) {
        if (appliedPower || appliedTouchpad) markApplyPending(false);
        return;
      }

      const kbdDev = getTargetDeviceForView('keyboard');
      if (!kbdDev || !kbdDev.lights || kbdDev.lights.length === 0) {
        if (appliedPower || appliedTouchpad) markApplyPending(false);
        return;
      }

      const matched = [];
      keyTargets.forEach((id) => {
        const keyDef = KEY_BY_ID.get(id);
        if (keyDef) {
          const lids = findLightsForKey(keyDef, kbdDev);
          if (lids.length > 0 || !isPerKeyKeyboard(kbdDev)) {
            state.keyBaseColors[id] = baseColor;
            state.keyBrightness[id] = state.zoneBrightness;
            state.keyMeta[id] = { ...metaObj };
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
      markApplyPending(false);
      return;
    }

    // Mouse or Chassis views (non-power zones)
    const zoneTargets = uiTargets.filter((zid) => zid !== 'power');
    if (zoneTargets.length === 0) {
      if (appliedPower) markApplyPending(false);
      return;
    }

    const viewDev = getTargetDeviceForView(state.activeView);
    if (!viewDev || !viewDev.lights || viewDev.lights.length === 0) {
      const labelEl = document.getElementById(state.activeView === 'mouse' ? 'mouse-zone-label' : 'chassis-zone-label');
      if (labelEl) labelEl.textContent = `No compatible ${state.activeView} device detected`;
      if (appliedPower) markApplyPending(false);
      return;
    }

    const matched = [];
    zoneTargets.forEach((zid) => {
      const zEl = document.querySelector(`.${state.activeView}-zone[data-zone-id="${zid}"]`);
      const zName = zEl ? zEl.dataset.zoneName : '';
      const lids = findLightsForZone(zid, zName, viewDev);
      if (lids.length > 0 && zEl) {
        const zKey = getZoneMetaKey(state.activeView, zid);
        state.zoneBaseColors[zKey] = baseColor;
        state.zoneBrightnessMap[zKey] = state.zoneBrightness;
        state.zoneMeta[zKey] = { ...metaObj };
        applySvgZoneVisualStyle(zEl, visColor);
        matched.push(...lids);
      }
    });
    const resolvedIds = Array.from(new Set(matched));
    if (resolvedIds.length === 0) {
      if (appliedPower) markApplyPending(false);
      return;
    }

    sendCommand({
      cmd: 'applyZoneColor',
      pid: viewDev.pid,
      lightIds: resolvedIds,
      zoneName: `${state.activeView.toUpperCase()} Zone`,
      ...commonPayload
    });
    markApplyPending(false);
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
    updateWavesModeUI();
    syncVirtualLightingFromState();
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
    const esc = (s) => String(s ?? '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
    const typeBadgeMap = {
      system: '💻 SYSTEM',
      mouse: '🖱 MOUSE',
      keyboard: '⌨ KEYBOARD',
      headset: '🎧 HEADSET',
      controller: '🎮 CONTROLLER',
      bluetooth: '📶 BLUETOOTH',
      hid: '🔌 HID DEVICE'
    };
    const grid = document.getElementById('battery-grid');
    grid.innerHTML = '';
    if (data.batteries.length === 0) {
      grid.innerHTML = '<div class="card">No battery-powered devices detected.</div>';
    } else {
      data.batteries.forEach((b) => {
        const isSleep = typeof b.percentage !== 'number' || b.percentage < 0;
        const pctWidth = isSleep ? 0 : Math.max(0, Math.min(100, Math.round(b.percentage)));
        const isLowAlert = !!b.lowBattAlert || (!isSleep && b.type === 'mouse' && pctWidth <= 15 && !b.charging);
        const card = document.createElement('div');
        card.className = `batt-card${isSleep ? ' sleep' : ''}${isLowAlert ? ' low-batt-alert' : ''}`;
        const pctLabel = isSleep ? 'Sleep' : `${pctWidth}%`;
        const typeLabel = typeBadgeMap[b.type] || esc((b.type || 'device').toUpperCase());
        const statusLabel = isSleep
          ? (b.charging ? '⚡ Charging (Sleep)' : 'Wireless Device Asleep (Deep Sleep)')
          : (isLowAlert
            ? '⚠ Low Battery (≤ 15%) — Mouse LEDs set to Red Alert'
            : (b.charging ? '⚡ Charging' : (b.acOnline ? 'Plugged In' : 'On Battery')));
        card.innerHTML = `
          <div class="batt-top">
            <div>
              <div class="batt-type">${typeLabel}</div>
              <strong>${esc(b.name)}</strong>
            </div>
            <div class="batt-pct accent">${pctLabel}</div>
          </div>
          <div class="batt-bar"><div class="batt-fill" style="width:${pctWidth}%"></div></div>
          <div class="hint-text">${statusLabel}</div>
        `;
        grid.appendChild(card);
      });
    }
    if (!data.batteries.some((b) => b.type !== 'system')) {
      const info = document.createElement('div');
      info.className = 'card';
      info.innerHTML = `
        <div class="batt-type">PERIPHERAL TELEMETRY INFO</div>
        <p class="hint-text" style="margin-top:6px;">No wireless mouse, controller, or headset battery detected yet. Ensure your Xbox, DualSense/DS4 controller, Astro/Logitech headset, Bluetooth, or AW610M 2.4GHz dongle is connected and awake, then click Scan Devices.</p>
      `;
      grid.appendChild(info);
    }
  }

  if (data.thermals) {
    if (typeof data.thermals.powerMode === 'number') {
      const isAuto = !!data.thermals.fanAuto;
      document.querySelectorAll('#power-mode-bar .seg-btn').forEach((b) => {
        const pm = b.dataset.pm;
        const active = (pm === 'auto')
          ? isAuto
          : (!isAuto && parseInt(pm, 10) === data.thermals.powerMode);
        b.classList.toggle('active', active);
      });
    }

    const sList = document.getElementById('sensor-list');
    sList.innerHTML = '';
    const sensors = data.thermals.sensors || [];
    state.lastSensors = sensors;
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

  const col1 = normalizeHex(chosenLs.color) || '#00f0ff';
  const col2 = normalizeHex(chosenLs.color2) || state.colorSecondary || '#7b2cbf';
  const col3 = normalizeHex(chosenLs.color3) || col1;
  let eff = chosenLs.effect || 'static';
  if (eff === 'summer' || eff === 'waves') eff = 'static';
  const isMouse = state.activeView === 'mouse';
  const isRear = !isMouse && isRearLightbarSelected();
  if (isRear && STD_TO_REAR_EFFECT[eff]) eff = STD_TO_REAR_EFFECT[eff];
  if (!isRear && REAR_TO_STD_EFFECT[eff]) eff = REAR_TO_STD_EFFECT[eff];
  if (isMouse && eff !== 'static' && eff !== 'morph') eff = 'static';
  const spd = typeof chosenLs.speed === 'number' ? Math.max(1, Math.min(100, chosenLs.speed)) : state.speed;
  const bright = typeof chosenLs.brightness === 'number' ? Math.max(5, Math.min(100, Math.round(chosenLs.brightness))) : 100;

  setPrimaryColorUI(col1);
  setZoneBrightnessUI(bright);
  state.colorSecondary = col2;
  state.effect = eff;
  state.speed = spd;
  if (state.effect === 'aurora' || state.effect === 'aurora_bar') {
    state.auroraColors = [col1, col2, col3];
    syncAuroraPaletteActive(col1, col2, col3);
  }

  document.getElementById('color-secondary').value = col2;
  document.getElementById('effect-speed').value = spd;
  syncLightingControlsForView();
  markApplyPending(false);
}

function syncVirtualLightingFromState() {
  const activeProf = state.profiles.find((p) => p.active) || state.profiles[0];
  const lightsOn = state.settings.lightsOn !== false;

  // Build exact (pid, lid) -> full lighting metadata & scaled color lookup from active profile lightsets
  const litBaseColorMap = new Map();
  const litBrightMap = new Map();
  const litColorMap = new Map();
  const litEffectMap = new Map();
  const litMetaMap = new Map();
  const deviceFallbackBase = new Map();
  const deviceFallbackBright = new Map();
  const deviceFallbackColor = new Map();
  const deviceFallbackMeta = new Map();
  const powerKeySet = new Set(findPowerButtonLights().map((p) => `${p.pid}:${p.lid}`));

  state.keyBaseColors = {};
  state.keyBrightness = {};
  state.keyMeta = {};
  state.zoneBaseColors = {};
  state.zoneBrightnessMap = {};
  state.zoneMeta = {};
  state.hwLightMeta.clear();

  if (activeProf && Array.isArray(activeProf.lightsets)) {
    activeProf.lightsets.forEach((ls) => {
      const baseCol = normalizeHex(ls.color) || '#00f0ff';
      const bright = typeof ls.brightness === 'number' ? Math.max(5, Math.min(100, Math.round(ls.brightness))) : 100;
      const visCol = scaleHexColor(baseCol, bright);
      const eff = ls.effect || 'static';
      const spd = typeof ls.speed === 'number' ? Math.max(1, Math.min(100, Math.round(ls.speed))) : 50;
      const col2 = normalizeHex(ls.color2) || '#7b2cbf';
      const col3 = normalizeHex(ls.color3) || baseCol;
      const metaObj = { color: baseCol, brightness: bright, effect: eff, speed: spd, color2: col2, color3: col3 };
      (ls.lights || []).forEach((gl) => {
        const key = `${gl.did}:${gl.lid}`;
        litBaseColorMap.set(key, baseCol);
        litBrightMap.set(key, bright);
        litMetaMap.set(key, metaObj);
        if (lightsOn) {
          litColorMap.set(key, visCol);
          litEffectMap.set(key, eff);
        }
        if (gl.did === state.selectedDevicePid) {
          state.hwLightMeta.set(gl.lid, { ...metaObj });
        }
        if (!powerKeySet.has(key) && !deviceFallbackBase.has(gl.did)) {
          deviceFallbackBase.set(gl.did, baseCol);
          deviceFallbackBright.set(gl.did, bright);
          deviceFallbackMeta.set(gl.did, metaObj);
          if (lightsOn) deviceFallbackColor.set(gl.did, visCol);
        }
      });
    });
  }

  // 1. Sync Keyboard & Numpad keys accurately against keyboard device
  const kbdDev = getTargetDeviceForView('keyboard');
  const kbdPerKey = isPerKeyKeyboard(kbdDev);
  const kbdFallback = kbdDev ? deviceFallbackColor.get(kbdDev.pid) : null;
  const kbdFallbackBase = kbdDev ? deviceFallbackBase.get(kbdDev.pid) : null;
  const kbdFallbackBright = kbdDev ? (deviceFallbackBright.get(kbdDev.pid) ?? 100) : 100;
  const kbdFallbackMeta = kbdDev ? deviceFallbackMeta.get(kbdDev.pid) : null;
  const pwrLights = findPowerButtonLights();

  document.querySelectorAll('.keycap[data-id]').forEach((keyEl) => {
    const keyId = keyEl.dataset.id;
    let col = null;
    let baseCol = null;
    let bright = 100;
    let eff = '';
    let foundMeta = null;
    if (keyId === 'power') {
      for (const pl of pwrLights) {
        const k = `${pl.pid}:${pl.lid}`;
        if (litBaseColorMap.has(k) && !baseCol) {
          baseCol = litBaseColorMap.get(k);
          bright = litBrightMap.get(k) ?? 100;
          foundMeta = litMetaMap.get(k) || null;
        }
        const hit = litColorMap.get(k);
        if (hit && !col) { col = hit; eff = litEffectMap.get(k) || ''; break; }
      }
    } else {
      const keyDef = KEY_BY_ID.get(keyId);
      if (keyDef && kbdDev) {
        const lids = findLightsForKey(keyDef, kbdDev);
        for (const lid of lids) {
          const k = `${kbdDev.pid}:${lid}`;
          if (litBaseColorMap.has(k) && !baseCol) {
            baseCol = litBaseColorMap.get(k);
            bright = litBrightMap.get(k) ?? 100;
            foundMeta = litMetaMap.get(k) || null;
          }
          const hit = litColorMap.get(k);
          if (hit) {
            col = hit;
            eff = litEffectMap.get(k) || '';
            break;
          }
        }
        if (!baseCol && !kbdPerKey && kbdFallbackBase) {
          baseCol = kbdFallbackBase;
          bright = kbdFallbackBright;
          foundMeta = kbdFallbackMeta;
        }
        if (!col && !kbdPerKey && kbdFallback) {
          col = kbdFallback;
        }
      }
    }
    if (baseCol) {
      state.keyBaseColors[keyId] = baseCol;
      state.keyBrightness[keyId] = bright;
      state.keyMeta[keyId] = foundMeta ? { ...foundMeta } : { color: baseCol, brightness: bright, effect: 'static', speed: 50, color2: state.colorSecondary, color3: baseCol };
    }
    if (keyId === 'power' && baseCol) {
      state.zoneBaseColors.power = baseCol;
      state.zoneBrightnessMap.power = bright;
      state.zoneMeta.power = { ...(state.keyMeta.power), effect: 'static' };
    }
    if (col) {
      state.keyColors[keyId] = col;
      applyKeyVisualStyle(keyEl, col, eff);
    } else {
      delete state.keyColors[keyId];
      applyKeyVisualStyle(keyEl, null);
    }
  });

  // Sync touchpad metadata if mapped
  const tpLights = findTouchpadLights();
  for (const tp of tpLights) {
    const tpMeta = litMetaMap.get(`${tp.pid}:${tp.lid}`);
    if (tpMeta) {
      state.zoneBaseColors.touchpad = tpMeta.color;
      state.zoneBrightnessMap.touchpad = tpMeta.brightness;
      state.zoneMeta.touchpad = { ...tpMeta };
      break;
    }
  }

  // 2. Sync Mouse zones
  const mouseDev = getTargetDeviceForView('mouse');
  const mouseSingleZone = mouseDev && (mouseDev.lights || []).filter((l) => (l.flags & 1) === 0).length <= 1;
  document.querySelectorAll('.mouse-zone').forEach((zoneEl) => {
    const zid = zoneEl.dataset.zoneId;
    let col = null;
    let baseCol = null;
    let bright = 100;
    let foundMeta = null;
    if (mouseDev) {
      const lids = findLightsForZone(zid, zoneEl.dataset.zoneName, mouseDev);
      for (const lid of lids) {
        const k = `${mouseDev.pid}:${lid}`;
        if (litBaseColorMap.has(k) && !baseCol) {
          baseCol = litBaseColorMap.get(k);
          bright = litBrightMap.get(k) ?? 100;
          foundMeta = litMetaMap.get(k) || null;
        }
        const hit = litColorMap.get(k);
        if (hit && !col) col = hit;
      }
      if (!baseCol && mouseSingleZone) {
        baseCol = deviceFallbackBase.get(mouseDev.pid) || null;
        bright = deviceFallbackBright.get(mouseDev.pid) ?? 100;
        foundMeta = deviceFallbackMeta.get(mouseDev.pid) || null;
      }
      if (!col && mouseSingleZone) col = deviceFallbackColor.get(mouseDev.pid) || null;
    }
    if (baseCol) {
      const zKey = getZoneMetaKey('mouse', zid);
      state.zoneBaseColors[zKey] = baseCol;
      state.zoneBrightnessMap[zKey] = bright;
      state.zoneMeta[zKey] = foundMeta ? { ...foundMeta } : { color: baseCol, brightness: bright, effect: 'static', speed: 50, color2: state.colorSecondary, color3: baseCol };
    }
    applySvgZoneVisualStyle(zoneEl, col);
  });

  // 4. Sync Chassis zones
  const chassisDev = getTargetDeviceForView('chassis');
  const chassisSingleZone = chassisDev && (chassisDev.lights || []).filter((l) => (l.flags & 1) === 0).length <= 1;
  document.querySelectorAll('.chassis-zone').forEach((zoneEl) => {
    const zid = zoneEl.dataset.zoneId;
    let col = null;
    let baseCol = null;
    let bright = 100;
    let foundMeta = null;
    if (zid === 'power') {
      for (const pl of pwrLights) {
        const k = `${pl.pid}:${pl.lid}`;
        if (litBaseColorMap.has(k) && !baseCol) {
          baseCol = litBaseColorMap.get(k);
          bright = litBrightMap.get(k) ?? 100;
          foundMeta = litMetaMap.get(k) || null;
        }
        const hit = litColorMap.get(k);
        if (hit && !col) col = hit;
      }
    } else if (chassisDev) {
      const lids = findLightsForZone(zid, zoneEl.dataset.zoneName, chassisDev);
      for (const lid of lids) {
        const k = `${chassisDev.pid}:${lid}`;
        if (litBaseColorMap.has(k) && !baseCol) {
          baseCol = litBaseColorMap.get(k);
          bright = litBrightMap.get(k) ?? 100;
          foundMeta = litMetaMap.get(k) || null;
        }
        const hit = litColorMap.get(k);
        if (hit && !col) col = hit;
      }
      if (!baseCol && chassisSingleZone) {
        baseCol = deviceFallbackBase.get(chassisDev.pid) || null;
        bright = deviceFallbackBright.get(chassisDev.pid) ?? 100;
        foundMeta = deviceFallbackMeta.get(chassisDev.pid) || null;
      }
      if (!col && chassisSingleZone) col = deviceFallbackColor.get(chassisDev.pid) || null;
    }
    if (baseCol) {
      const zKey = getZoneMetaKey('chassis', zid);
      state.zoneBaseColors[zKey] = baseCol;
      state.zoneBrightnessMap[zKey] = bright;
      state.zoneMeta[zKey] = foundMeta
        ? (zid === 'power' ? { ...foundMeta, effect: 'static' } : { ...foundMeta })
        : { color: baseCol, brightness: bright, effect: 'static', speed: 50, color2: state.colorSecondary, color3: baseCol };
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

function renderHardwareDiagnostics(data) {
  const panel = document.getElementById('hw-diag-panel');
  const out = document.getElementById('hw-diag-output');
  if (!panel || !out) return;
  const lines = [];
  if (data.wmi) {
    lines.push(`[WMI / ACPI] Alienware=${!!data.wmi.isAlienware} Supported=${!!data.wmi.isSupported} GMode=${!!data.wmi.isGmode} Fans=${data.wmi.fans} Sensors=${data.wmi.sensors} PowerModes=${data.wmi.powerModes}`);
  }
  if (Array.isArray(data.mappedDevices)) {
    lines.push(`[Mapped Devices: ${data.mappedDevices.length}]`);
    data.mappedDevices.forEach((m) => {
      lines.push(`  • ${m.name} (${m.vid}:${m.pid}) present=${m.present} API=v${m.version} v4MaxZones=${m.v4MaxZones} lights=${m.lightCount}`);
    });
  }
  if (Array.isArray(data.services)) {
    lines.push(`[Services: ${data.services.length}]`);
    data.services.forEach((s) => {
      lines.push(`  • ${s.name} (${s.displayName}) state=${s.state}`);
    });
  }
  if (Array.isArray(data.controllers)) {
    lines.push(`[HID Controllers: ${data.controllers.length}]`);
    data.controllers.forEach((c) => {
      lines.push(`  • ${c.vid}:${c.pid} UP=${c.usagePage} U=${c.usage} API=${c.api} in=${c.inLen} out=${c.outLen} feat=${c.featLen} err=${c.openError} | ${c.product || ''}`);
      if (c.probeInfo) lines.push(`    probe: ${c.probeInfo}`);
      if (c.path) lines.push(`    path: ${c.path}`);
    });
  }
  out.textContent = lines.join('\n') || JSON.stringify(data, null, 2);
  panel.style.display = 'flex';
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
    state.probeLedId = 0;
    sendCommand({ cmd: 'endProbe' });
  }
  updateProbeDeckUI();
}

function initLedProbeDeck() {
  const toggle = document.getElementById('probe-toggle');
  const devSel = document.getElementById('probe-dev-select');
  const prevBtn = document.getElementById('probe-prev');
  const nextBtn = document.getElementById('probe-next');
  const stepper = document.querySelector('.probe-stepper');
  const addBtn = document.getElementById('probe-add');
  const remBtn = document.getElementById('probe-remove');
  const tpBtn = document.getElementById('probe-bind-tp');
  const pwrBtn = document.getElementById('probe-bind-pwr');
  const btnEmergWhite = document.getElementById('btn-emergency-white');
  const btnDetectHw = document.getElementById('btn-detect-hardware');
  const btnCopyDiag = document.getElementById('btn-copy-diag');
  const btnCloseDiag = document.getElementById('btn-close-diag');

  const stepProbe = (delta) => {
    state.probeLedId = (state.probeLedId + delta + 256) % 256;
    updateProbeDeckUI();
    if (state.probeActive) sendProbeLed();
  };

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
    prevBtn.addEventListener('click', () => stepProbe(-1));
  }
  if (nextBtn) {
    nextBtn.addEventListener('click', () => stepProbe(1));
  }
  if (stepper) {
    stepper.addEventListener('wheel', (e) => {
      if (!state.probeActive) return;
      if (typeof e.preventDefault === 'function') e.preventDefault();
      const delta = (e.deltaY < 0 || e.deltaX > 0) ? 1 : -1;
      stepProbe(delta);
    }, { passive: false });
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
  if (btnEmergWhite) {
    btnEmergWhite.addEventListener('click', () => {
      if (state.probeActive) setProbeActive(false);
      sendCommand({ cmd: 'emergencyWhite' });
      const prevEl = document.getElementById('selected-names');
      if (prevEl) prevEl.textContent = '⚡ Emergency White broadcast sent to all controllers (0..255 chassis / keyboard / mouse)';
    });
  }
  if (btnDetectHw) {
    btnDetectHw.addEventListener('click', () => {
      const panel = document.getElementById('hw-diag-panel');
      const out = document.getElementById('hw-diag-output');
      if (panel) panel.style.display = 'flex';
      if (out) out.textContent = 'Scanning USB HID & ACPI hardware interfaces...';
      sendCommand({ cmd: 'detectHardware' });
    });
  }
  if (btnCopyDiag) {
    btnCopyDiag.addEventListener('click', () => {
      const out = document.getElementById('hw-diag-output');
      if (out && navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(out.textContent || '').catch(() => {});
      }
    });
  }
  if (btnCloseDiag) {
    btnCloseDiag.addEventListener('click', () => {
      const panel = document.getElementById('hw-diag-panel');
      if (panel) panel.style.display = 'none';
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
    state.wavesMode = !!(data.settings.wavesMode ?? data.settings.summerMode);
    if (Array.isArray(data.settings.favorites)) {
      state.favorites = Array.from(new Set(data.settings.favorites.map(normalizeHex).filter(Boolean)));
    } else if (!hasWebView) {
      state.favorites = loadFavoritesFallback();
    } else {
      state.favorites = [];
    }
    updateLightsButton();
    updateWavesModeUI();
    renderFavoriteColors();
    document.getElementById('master-brightness').value = data.settings.brightness ?? 255;
    document.getElementById('master-brightness-val').textContent = `${Math.round(((data.settings.brightness ?? 255) / 255) * 100)}%`;
    document.getElementById('set-start-windows').checked = !!data.settings.startWindows;
    document.getElementById('set-start-minimized').checked = !!data.settings.startMinimized;
    const setAlwaysAdmin = document.getElementById('set-always-admin');
    if (setAlwaysAdmin) setAlwaysAdmin.checked = !!data.settings.alwaysAdmin;
    const setStartupIntro = document.getElementById('set-startup-intro');
    if (setStartupIntro) setStartupIntro.checked = data.settings.startupIntro !== false;
    const setIntroStyle = document.getElementById('set-intro-style');
    if (setIntroStyle) setIntroStyle.value = String(data.settings.introStyle ?? 1);
    const setIntroText = document.getElementById('set-intro-text');
    if (setIntroText && data.settings.introText !== undefined) setIntroText.value = data.settings.introText || 'COCKPIT';
    const introSpeedLabels = ['Very Slow', 'Slow', 'Medium', 'Fast', 'Very Fast'];
    const spdIdx = Math.max(0, Math.min(4, data.settings.introSpeed ?? 2));
    const setIntroSpeed = document.getElementById('set-intro-speed');
    if (setIntroSpeed) setIntroSpeed.value = String(spdIdx);
    const setIntroSpeedVal = document.getElementById('set-intro-speed-val');
    if (setIntroSpeedVal) setIntroSpeedVal.textContent = introSpeedLabels[spdIdx];
    const setLowBatt = document.getElementById('set-low-batt-alert');
    if (setLowBatt) setLowBatt.checked = data.settings.lowBatteryAlert !== false;
    const wavesColorEl = document.getElementById('waves-color-picker');
    if (wavesColorEl && data.settings.wavesColor) wavesColorEl.value = data.settings.wavesColor;
    const wavesBrightEl = document.getElementById('waves-brightness');
    const wavesBrightValEl = document.getElementById('waves-brightness-val');
    const wb = data.settings.wavesBrightness ?? 100;
    if (wavesBrightEl) wavesBrightEl.value = String(wb);
    if (wavesBrightValEl) wavesBrightValEl.textContent = `${wb}%`;
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
  syncVirtualLightingFromState();
  renderHardwareLights();
  updateProbeDeckUI();
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
  const escHtml = (s) => String(s || '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  state.profiles.forEach((p) => {
    if (p.isPreset) return;
    if (p.active && !activePreset) document.getElementById('hud-profile').textContent = p.name;
    const card = document.createElement('div');
    card.className = `profile-card ${p.active ? 'active' : ''}`;
    const safeName = escHtml(p.name);
    card.innerHTML = `
      <div>
        <div class="profile-header-row" id="prof-name-view-${p.id}">
          <strong>${safeName}</strong>
          <button type="button" class="qs-btn" title="Rename profile" onclick="startRenameProf(${p.id})">&#9998; Rename</button>
        </div>
        <div class="profile-rename-row" id="prof-name-edit-${p.id}" style="display:none;">
          <input type="text" class="profile-rename-input" id="prof-rename-input-${p.id}" value="${safeName}" maxlength="48" onkeydown="if(event.key==='Enter')saveRenameProf(${p.id});else if(event.key==='Escape')cancelRenameProf(${p.id});" />
          <button type="button" class="btn-secondary" onclick="saveRenameProf(${p.id})">Save</button>
          <button type="button" class="qs-btn" onclick="cancelRenameProf(${p.id})">Cancel</button>
        </div>
        <div class="hint-text">${p.isDefault ? 'System Default' : 'Custom Profile'}</div>
      </div>
      <div class="profile-actions">
        <button class="btn-secondary" onclick="activateProf(${p.id})">${p.active ? 'Active' : 'Activate'}</button>
        <button class="qs-btn" onclick="duplicateProf(${p.id})">Duplicate</button>
        ${!p.isDefault ? `<button class="qs-btn" onclick="deleteProf(${p.id})">Delete</button>` : ''}
      </div>
    `;
    pGrid.appendChild(card);
  });
  if (activePreset) {
    document.getElementById('hud-profile').textContent = `Preset: ${activePreset}`;
  }
  markApplyPending(false);
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
      const added = !state.selectedHwLights.has(l.id);
      if (added) state.selectedHwLights.add(l.id);
      else state.selectedHwLights.delete(l.id);
      refreshSelectionUI();
      if (added) {
        if (state.selectedHwLights.size === 1) {
          loadSelectionColorAndBrightness();
        } else {
          syncLightingControlsForView();
        }
      } else if (state.selectedHwLights.size > 0) {
        loadSelectionColorAndBrightness();
      } else {
        markApplyPending(false);
        syncLightingControlsForView();
      }
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
window.duplicateProf = (id) => sendCommand({ cmd: 'duplicateProfile', id });
window.startRenameProf = (id) => {
  const viewRow = document.getElementById(`prof-name-view-${id}`);
  const editRow = document.getElementById(`prof-name-edit-${id}`);
  const inp = document.getElementById(`prof-rename-input-${id}`);
  if (viewRow) viewRow.style.display = 'none';
  if (editRow) editRow.style.display = 'flex';
  if (inp) {
    if (typeof inp.focus === 'function') inp.focus();
    if (typeof inp.select === 'function') inp.select();
  }
};
window.cancelRenameProf = (id) => {
  const viewRow = document.getElementById(`prof-name-view-${id}`);
  const editRow = document.getElementById(`prof-name-edit-${id}`);
  if (editRow) editRow.style.display = 'none';
  if (viewRow) viewRow.style.display = 'flex';
};
window.saveRenameProf = (id) => {
  const inp = document.getElementById(`prof-rename-input-${id}`);
  const name = inp ? inp.value.trim() : '';
  if (!name) return;
  sendCommand({ cmd: 'renameProfile', id, name });
};

function initWindowControls() {
  const minBtn = document.getElementById('win-min');
  const maxBtn = document.getElementById('win-max');
  const closeBtn = document.getElementById('win-close');
  const titlebar = document.getElementById('titlebar');

  if (minBtn) minBtn.addEventListener('click', () => sendCommand({ cmd: 'windowMinimize' }));
  if (maxBtn) maxBtn.addEventListener('click', () => sendCommand({ cmd: 'windowMaximize' }));
  if (closeBtn) closeBtn.addEventListener('click', () => sendCommand({ cmd: 'windowClose' }));

  if (titlebar) {
    const supportsAppRegion = window.CSS && CSS.supports &&
      (CSS.supports('app-region', 'drag') || CSS.supports('-webkit-app-region', 'drag'));
    if (!supportsAppRegion) {
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
}

function initMiscActions() {
  document.getElementById('hw-device-dropdown').addEventListener('change', (e) => {
    state.selectedDevicePid = parseInt(e.target.value, 10);
    const pdd = document.getElementById('probe-dev-select');
    if (pdd) pdd.value = String(state.selectedDevicePid);
    state.selectedHwLights.clear();
    syncVirtualLightingFromState();
    renderHardwareLights();
    refreshSelectionUI();
    updateProbeDeckUI();
    markApplyPending(false);
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
      if (state.selectedHwLights.size > 0) loadSelectionColorAndBrightness();
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

  const btnOpenProfiles = document.getElementById('btn-open-profiles-folder');
  if (btnOpenProfiles) {
    btnOpenProfiles.addEventListener('click', () => {
      sendCommand({ cmd: 'openProfilesLocation' });
    });
  }

  document.querySelectorAll('#power-mode-bar .seg-btn').forEach((btn) => {
    btn.addEventListener('click', () => {
      document.querySelectorAll('#power-mode-bar .seg-btn').forEach((b) => b.classList.remove('active'));
      btn.classList.add('active');
      if (btn.dataset.pm === 'auto') {
        sendCommand({ cmd: 'setFanAuto' });
      } else {
        sendCommand({ cmd: 'setPowerMode', mode: parseInt(btn.dataset.pm, 10) });
      }
    });
  });

  const getPrimarySensorId = () => (Array.isArray(state.lastSensors) && state.lastSensors.length > 0 ? (state.lastSensors[0].id ?? 0) : 0);

  document.querySelectorAll('[data-tpreset]').forEach((btn) => {
    btn.addEventListener('click', () => {
      const presetKey = btn.dataset.tpreset;
      const preset = THERMAL_PRESETS[presetKey];
      if (!preset) return;
      state.fanCurve = preset.points.map((p) => ({ ...p }));
      renderFanCurve();
      updateCurvePresetBadge();
      if (presetKey === 'max') {
        document.querySelectorAll('#power-mode-bar .seg-btn').forEach((b) => {
          b.classList.toggle('active', parseInt(b.dataset.pm, 10) === 3);
        });
        sendCommand({ cmd: 'setFanCurve', fanId: -1, sensorId: getPrimarySensorId(), active: false, points: state.fanCurve });
        sendCommand({ cmd: 'setPowerMode', mode: 3 });
      } else {
        document.querySelectorAll('#power-mode-bar .seg-btn').forEach((b) => {
          b.classList.toggle('active', b.dataset.pm === 'auto');
        });
        sendCommand({ cmd: 'setFanCurve', fanId: -1, sensorId: getPrimarySensorId(), active: true, points: state.fanCurve });
      }
    });
  });

  const gamePresets = {
    cyberpunk: { color: '#fcee0a', color2: '#00f0ff', color3: '#ff007f', effect: 'aurora', speed: 60, name: 'Cyberpunk' },
    aurora: { color: '#00f0ff', color2: '#00ff9d', color3: '#7b2cbf', effect: 'aurora', speed: 45, name: 'Aurora Borealis' },
    area51: { color: '#00f0ff', color2: '#7b2cbf', color3: '#3a86ff', effect: 'morph', speed: 50, name: 'Area-51 Orbital' },
    stealth: { color: '#ff2a4b', color2: '#7b2cbf', color3: '#ff2a4b', effect: 'breathing', speed: 35, name: 'Crimson Stealth' },
    spectrum: { color: '#ff007f', color2: '#3a86ff', color3: '#00ff9d', effect: 'spectrum', speed: 55, name: 'Rainbow Spectrum' }
  };

  document.querySelectorAll('#game-presets .preset-chip').forEach((btn) => {
    btn.addEventListener('click', () => {
      const p = gamePresets[btn.dataset.preset];
      if (!p) return;
      setPrimaryColorUI(p.color);
      state.colorSecondary = p.color2;
      state.auroraColors = [p.color, p.color2, p.color3];
      state.effect = p.effect;
      state.speed = p.speed;
      document.getElementById('color-secondary').value = p.color2;
      document.getElementById('effect-speed').value = p.speed;
      syncLightingControlsForView();
      markApplyPending(false);

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
    document.querySelectorAll('#power-mode-bar .seg-btn').forEach((b) => {
      b.classList.toggle('active', b.dataset.pm === 'auto');
    });
    sendCommand({ cmd: 'setFanCurve', fanId: -1, sensorId: getPrimarySensorId(), active: true, points: state.fanCurve });
  });

  document.getElementById('btn-refresh-batt').addEventListener('click', () => {
    sendCommand({ cmd: 'getTelemetry' });
  });

  const setIntroStyle = document.getElementById('set-intro-style');
  const setIntroText = document.getElementById('set-intro-text');
  const setIntroSpeed = document.getElementById('set-intro-speed');
  const setIntroSpeedVal = document.getElementById('set-intro-speed-val');
  const introSpeedLabels = ['Very Slow', 'Slow', 'Medium', 'Fast', 'Very Fast'];

  const parseSpeedIdx = (raw) => {
    const parsed = parseInt(raw, 10);
    return Number.isNaN(parsed) ? 2 : Math.max(0, Math.min(4, parsed));
  };

  if (setIntroStyle) {
    setIntroStyle.addEventListener('change', (e) => {
      const parsed = parseInt(e.target.value, 10);
      const style = Number.isNaN(parsed) ? 1 : parsed;
      state.settings.introStyle = style;
      sendCommand({ cmd: 'setSettings', introStyle: style });
    });
  }
  if (setIntroText) {
    setIntroText.addEventListener('input', (e) => {
      const cleaned = (e.target.value || '').replace(/[^a-zA-Z0-9]/g, '').slice(0, 10);
      if (e.target.value !== cleaned) e.target.value = cleaned;
    });
    setIntroText.addEventListener('change', (e) => {
      const text = (e.target.value || '').replace(/[^a-zA-Z0-9]/g, '').slice(0, 10) || 'COCKPIT';
      e.target.value = text;
      state.settings.introText = text;
      sendCommand({ cmd: 'setSettings', introText: text });
    });
  }
  if (setIntroSpeed) {
    setIntroSpeed.addEventListener('input', (e) => {
      const idx = parseSpeedIdx(e.target.value);
      if (setIntroSpeedVal) setIntroSpeedVal.textContent = introSpeedLabels[idx];
    });
    setIntroSpeed.addEventListener('change', (e) => {
      const idx = parseSpeedIdx(e.target.value);
      state.settings.introSpeed = idx;
      if (setIntroSpeedVal) setIntroSpeedVal.textContent = introSpeedLabels[idx];
      sendCommand({ cmd: 'setSettings', introSpeed: idx });
    });
  }

  const btnTestIntro = document.getElementById('btn-test-intro');
  if (btnTestIntro) {
    btnTestIntro.addEventListener('click', () => {
      const styleParsed = setIntroStyle ? parseInt(setIntroStyle.value, 10) : 1;
      const style = Number.isNaN(styleParsed) ? 1 : styleParsed;
      const speed = setIntroSpeed ? parseSpeedIdx(setIntroSpeed.value) : 2;
      const text = setIntroText ? ((setIntroText.value || '').replace(/[^a-zA-Z0-9]/g, '').slice(0, 10) || 'COCKPIT') : 'COCKPIT';
      sendCommand({ cmd: 'testIntro', style, speed, text });
    });
  }

  const linkAuthorSite = document.getElementById('link-author-site');
  if (linkAuthorSite) {
    linkAuthorSite.addEventListener('click', (e) => {
      e.preventDefault();
      sendCommand({ cmd: 'openExternalUrl', url: 'https://ngservers.net' });
    });
  }

  const settingMap = {
    'set-start-windows': 'startWindows',
    'set-start-minimized': 'startMinimized',
    'set-always-admin': 'alwaysAdmin',
    'set-startup-intro': 'startupIntro',
    'set-low-batt-alert': 'lowBatteryAlert',
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
      if (key === 'lowBatteryAlert' && e.target.checked) {
        sendCommand({ cmd: 'previewLowBatteryAlert' });
      }
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
  updateWavesModeUI();
  renderFavoriteColors();
  syncLightingControlsForView();

  if (hasWebView) {
    sendCommand({ cmd: 'getState' });
    sendCommand({ cmd: 'getTelemetry' });
  } else {
    applyFullState({
      devices: [{ pid: 0x550, name: 'Alienware Integrated Keyboard (Preview)', lights: [{ id: 1, name: 'WASD Cluster' }, { id: 2, name: 'Logo Emblem' }] }],
      profiles: [{ id: 0, name: 'Default', isDefault: true, active: true }, { id: 1, name: 'Orbital Gaming', isDefault: false, active: false }],
      settings: { lightsOn: true, brightness: 255, startWindows: true, startMinimized: true, startupIntro: true, introStyle: 0, wavesMode: false, favorites: ['#00f0ff', '#fcee0a'] }
    });
    applyTelemetry({
      system: { cpu: 24, temp: 54, batt: 88 },
      batteries: [
        { name: 'System Battery', type: 'system', percentage: 88, charging: true, acOnline: true },
        { name: 'Alienware AW610M Wireless Mouse', type: 'mouse', percentage: 74, charging: false, acOnline: false }
      ],
      thermals: {
        sensors: [{ name: 'CPU Package', temp: 54 }, { name: 'GPU Core', temp: 48 }],
        fans: [{ id: 0, rpm: 2450, percent: 45 }, { id: 1, rpm: 2300, percent: 42 }]
      }
    });
  }
});
