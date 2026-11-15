(() => {
  'use strict';

  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
  const pad = (value) => String(value).padStart(2, '0');
  const clamp = (value, min, max) => Math.min(max, Math.max(min, value));
  const money = (value) => Number(value).toLocaleString('zh-CN');
  const escapeHtml = (value) => String(value)
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&#39;');

  const TRACKS = [
    { title: '清晨微光', artist: 'WAVE 原创曲库', duration: 214 },
    { title: '海风来信', artist: 'WAVE 原创曲库', duration: 236 },
    { title: '星夜漫步', artist: 'WAVE 原创曲库', duration: 198 },
  ];

  const FACES = {
    luxe: { name: '鎏金雅韵', short: '鎏金', label: '鎏金雅韵' },
    sport: { name: '活力运动', short: '运动', label: '活力运动' },
    classic: { name: '经典指针', short: '经典', label: '经典指针' },
  };

  const WEEKDAYS = ['周日', '周一', '周二', '周三', '周四', '周五', '周六'];
  const PERSIST_KEY = 'wave-lab-device-v1';
  const LOW_BATTERY = 10;

  const els = {
    faceView: $('#faceView'),
    screen: $('#screen'),
    pageLayer: $('#pageLayer'),
    assistantOverlay: $('#assistantOverlay'),
    assistantStatus: $('#assistantStatus'),
    assistantText: $('#assistantText'),
    assistantCaption: $('#assistantCaption'),
    assistantEnd: $('#assistantEnd'),
    assistantEyes: $('#assistantEyes'),
    toast: $('#toast'),
    toastText: $('#toastText'),
    screenInner: $('#screenInner'),
    screenDim: $('#screenDim'),
    stage: $('#stage'),
    deviceWrap: $('#deviceWrap'),
    device: $('#device'),
    configSource: $('#configSource'),
    statBattery: $('#statBattery'),
    statSteps: $('#statSteps'),
    statVoice: $('#statVoice'),
    statPage: $('#statPage'),
    batteryRange: $('#batteryRange'),
    brightnessRange: $('#brightnessRange'),
    volumeRange: $('#volumeRange'),
    batteryOut: $('#batteryOut'),
    brightnessOut: $('#brightnessOut'),
    volumeOut: $('#volumeOut'),
    wifiToggle: $('#wifiToggle'),
    sdToggle: $('#sdToggle'),
    imuToggle: $('#imuToggle'),
    dndToggle: $('#dndToggle'),
    autoStepsToggle: $('#autoStepsToggle'),
    soundToggle: $('#soundToggle'),
    chargeToggle: $('#chargeToggle'),
    stepAdd: $('#stepAdd'),
    batteryDrop: $('#batteryDrop'),
    virtualClock: $('#virtualClock'),
    virtualDate: $('#virtualDate'),
    clockSpeedLabel: $('#clockSpeedLabel'),
    pauseClock: $('#pauseClock'),
    jumpHour: $('#jumpHour'),
    resetClock: $('#resetClock'),
    jumpTime: $('#jumpTime'),
    applyTime: $('#applyTime'),
    nextAlarm: $('#nextAlarm'),
    commandInput: $('#commandInput'),
    runCommand: $('#runCommand'),
    lastResponse: $('#lastResponse'),
    pairState: $('#pairState'),
    pairIp: $('#pairIp'),
    pairCode: $('#pairCode'),
    pairCountdown: $('#pairCountdown'),
    pairInput: $('#pairInput'),
    openPairing: $('#openPairing'),
    copyPair: $('#copyPair'),
    simulatePair: $('#simulatePair'),
    syncConfig: $('#syncConfig'),
    pushConfig: $('#pushConfig'),
    font30Toggle: $('#font30Toggle'),
    classicDarkToggle: $('#classicDarkToggle'),
    hideNotifyToggle: $('#hideNotifyToggle'),
    copyState: $('#copyState'),
    stateJson: $('#stateJson'),
    eventLog: $('#eventLog'),
    clearLog: $('#clearLog'),
    gestureNote: $('#gestureNote'),
    prevFace: $('#prevFace'),
    nextFace: $('#nextFace'),
    voiceButton: $('#voiceButton'),
  };

  const saved = readSavedState();
  const savedNumber = (value, fallback) =>
    value !== null && value !== undefined && value !== '' && Number.isFinite(Number(value))
      ? Number(value)
      : fallback;
  const state = {
    face: saved.face || 'luxe',
    page: 'face',
    brightness: clamp(savedNumber(saved.brightness, 72), 10, 100),
    volume: clamp(savedNumber(saved.volume, 55), 0, 100),
    dnd: Boolean(saved.dnd),
    battery: clamp(savedNumber(saved.battery, 87), 0, 100),
    charging: Boolean(saved.charging),
    steps: Math.max(0, savedNumber(saved.steps, 6842)),
    goal: Math.max(1000, savedNumber(saved.goal, 8000)),
    wifi: saved.wifi !== false,
    sd: saved.sd !== false,
    imu: saved.imu !== false,
    autoSteps: Boolean(saved.autoSteps),
    sound: saved.sound !== false,
    music: {
      track: Number.isInteger(saved.music?.track) ? clamp(saved.music.track, 0, TRACKS.length - 1) : 0,
      status: 'paused',
      position: clamp(savedNumber(saved.music?.position, 0), 0, TRACKS[clamp(savedNumber(saved.music?.track, 0), 0, TRACKS.length - 1)].duration),
      autoPaused: false,
    },
    alarm: {
      hour: clamp(savedNumber(saved.alarm?.hour, 7), 0, 23),
      minute: clamp(savedNumber(saved.alarm?.minute, 0), 0, 59),
      enabled: Boolean(saved.alarm?.enabled),
      nextAt: 0,
    },
    timer: { active: false, endsAt: 0, expired: false, duration: 0 },
    pairing: { active: false, connected: false, expired: false, code: '', ip: '192.168.1.23', expiresAt: 0 },
    assistant: { visible: false, status: 'idle', caption: '', autoPaused: false },
    notification: { text: '', type: '', until: 0 },
    font30: Boolean(saved.font30),
    classicDark: Boolean(saved.classicDark),
    hideNotify: Boolean(saved.hideNotify),
    log: [],
    source: 'local',
  };

  const clock = {
    virtualMs: Date.now(),
    speed: 1,
    paused: false,
    lastReal: performance.now(),
    lastDateKey: dateKey(new Date()),
  };

  let audioContext = null;
  let assistantIdleTimer = null;
  let assistantCloseTimer = null;
  let toastTimer = null;
  let configSaveTimer = null;
  let persistTimer = null;
  let lastFrame = performance.now();
  let lowBatteryAlerted = false;

  // ----------------------------- utilities -----------------------------

  function readSavedState() {
    try {
      return JSON.parse(localStorage.getItem(PERSIST_KEY) || '{}') || {};
    } catch {
      return {};
    }
  }

  function persistState() {
    clearTimeout(persistTimer);
    persistTimer = setTimeout(() => {
      const data = {
        face: state.face,
        brightness: state.brightness,
        volume: state.volume,
        dnd: state.dnd,
        battery: state.battery,
        charging: state.charging,
        steps: state.steps,
        goal: state.goal,
        wifi: state.wifi,
        sd: state.sd,
        imu: state.imu,
        autoSteps: state.autoSteps,
        sound: state.sound,
        font30: state.font30,
        classicDark: state.classicDark,
        hideNotify: state.hideNotify,
        music: { track: state.music.track, position: Math.floor(state.music.position) },
        alarm: { hour: state.alarm.hour, minute: state.alarm.minute, enabled: state.alarm.enabled },
      };
      try { localStorage.setItem(PERSIST_KEY, JSON.stringify(data)); } catch { /* ignore */ }
    }, 180);
  }

  function dateKey(date) {
    return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}`;
  }

  function formatDate(date) {
    return `${date.getMonth() + 1}月${date.getDate()}日 ${WEEKDAYS[date.getDay()]}`;
  }

  function formatClock(date) {
    return `${pad(date.getHours())}:${pad(date.getMinutes())}`;
  }

  function formatSeconds(totalSeconds) {
    const seconds = Math.max(0, Math.floor(totalSeconds));
    return `${pad(Math.floor(seconds / 3600))}:${pad(Math.floor((seconds % 3600) / 60))}:${pad(seconds % 60)}`;
  }

  function formatMediaTime(totalSeconds) {
    const seconds = Math.max(0, Math.floor(totalSeconds));
    return `${pad(Math.floor(seconds / 60))}:${pad(seconds % 60)}`;
  }

  function formatTimeOfDay(hour, minute) {
    return `${pad(hour)}:${pad(minute)}`;
  }

  function addLog(type, message) {
    state.log.unshift({
      at: new Date(),
      type,
      message,
    });
    if (state.log.length > 80) state.log.length = 80;
    renderLog();
  }

  function renderLog() {
    if (!els.eventLog) return;
    els.eventLog.innerHTML = state.log.map((item) => {
      const time = `${pad(item.at.getHours())}:${pad(item.at.getMinutes())}:${pad(item.at.getSeconds())}`;
      return `<div class="log-entry" data-type="${escapeHtml(item.type)}">
        <time>${time}</time><span class="type">${escapeHtml(item.type)}</span>
        <span class="msg">${escapeHtml(item.message)}</span>
      </div>`;
    }).join('');
    els.eventLog.scrollTop = 0;
  }

  function notify(message, type = 'system', duration = 5600) {
    if (type === 'alarm' && !state.dnd) playAlarm();
    else if (type === 'timer' && !state.dnd) playTimer();
    if (state.hideNotify) {
      addLog('system', `[通知被表盘遮挡] ${message}`);
      return;
    }
    state.notification = {
      text: message,
      type,
      until: performance.now() + duration,
    };
    if (!els.toast) return;
    els.toast.className = `toast ${type}`;
    els.toastText.textContent = message;
    els.toast.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => {
      els.toast.hidden = true;
    }, duration);
  }

  function ensureAudio() {
    if (!state.sound) return null;
    try {
      const AudioCtor = window.AudioContext || window.webkitAudioContext;
      if (!AudioCtor) return null;
      audioContext ||= new AudioCtor();
      if (audioContext.state === 'suspended') audioContext.resume().catch(() => {});
      return audioContext;
    } catch {
      return null;
    }
  }

  function playTone(freq, start, duration, gainValue = 0.035) {
    const context = ensureAudio();
    if (!context) return;
    const oscillator = context.createOscillator();
    const gain = context.createGain();
    oscillator.type = 'sine';
    oscillator.frequency.value = freq;
    gain.gain.setValueAtTime(0, start);
    gain.gain.linearRampToValueAtTime(gainValue, start + 0.01);
    gain.gain.exponentialRampToValueAtTime(0.0001, start + duration);
    oscillator.connect(gain).connect(context.destination);
    oscillator.start(start);
    oscillator.stop(start + duration + 0.02);
  }

  function playAlarm() {
    const context = ensureAudio();
    if (!context) return;
    const start = context.currentTime + 0.02;
    [0, 0.22, 0.52, 0.74].forEach((offset, index) => {
      playTone(index % 2 === 0 ? 880 : 660, start + offset, 0.14, 0.045);
    });
  }

  function playTimer() {
    const context = ensureAudio();
    if (!context) return;
    const start = context.currentTime + 0.02;
    playTone(760, start, 0.12, 0.035);
    playTone(980, start + 0.16, 0.18, 0.035);
  }

  // ----------------------------- config sync -----------------------------

  function setConfigSource(source) {
    state.source = source;
    if (source === 'api') {
      els.configSource.classList.remove('local');
      els.configSource.querySelector('span').textContent = '本机配置服务';
    } else {
      els.configSource.classList.add('local');
      els.configSource.querySelector('span').textContent = 'localStorage';
    }
  }

  function configPayload() {
    return {
      face: state.face,
      brightness: state.brightness,
      volume: state.volume,
      doNotDisturb: state.dnd,
      assistantEnabled: true,
      wakeWord: '小智小智',
      language: 'zh-CN',
      complications: { weather: true, steps: true, battery: true },
    };
  }

  function applyConfig(config, source, silent = true) {
    if (!config || typeof config !== 'object') return;
    let faceChanged = false;
    if (Object.hasOwn(FACES, config.face) && config.face !== state.face) {
      state.face = config.face;
      faceChanged = true;
    }
    if (Number.isInteger(config.brightness)) state.brightness = clamp(config.brightness, 10, 100);
    if (Number.isInteger(config.volume)) state.volume = clamp(config.volume, 0, 100);
    if (typeof config.doNotDisturb === 'boolean') state.dnd = config.doNotDisturb;
    setConfigSource(source);
    if (faceChanged) buildFace();
    syncControls();
    renderFaceDynamic();
    renderPageDynamic();
    persistState();
    if (!silent) addLog('config', `已从配置中心读取：${FACES[state.face].name} / ${state.brightness}% / ${state.volume}%${state.dnd ? ' / 勿扰' : ''}`);
  }

  async function loadSharedConfig(silent = true) {
    try {
      const response = await fetch('/api/config', { cache: 'no-store' });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const config = await response.json();
      applyConfig(config, 'api', silent);
      return true;
    } catch {
      setConfigSource('local');
      return false;
    }
  }

  function scheduleConfigSave() {
    clearTimeout(configSaveTimer);
    configSaveTimer = setTimeout(async () => {
      try {
        const response = await fetch('/api/config', {
          method: 'PUT',
          headers: { 'content-type': 'application/json' },
          body: JSON.stringify(configPayload()),
        });
        if (!response.ok) throw new Error('save failed');
        setConfigSource('api');
      } catch {
        setConfigSource('local');
      }
    }, 650);
  }

  async function pushConfigToPortal() {
    let savedOk = false;
    try {
      const response = await fetch('/api/config', {
        method: 'PUT',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify(configPayload()),
      });
      savedOk = response.ok;
    } catch {
      savedOk = false;
    }
    if (savedOk) {
      setConfigSource('api');
      addLog('config', '已保存表盘、亮度、音量和勿扰到配置中心');
      notify('配置已保存到配置中心', 'system');
    } else {
      setConfigSource('local');
      addLog('config', '配置中心不可用，已保存到 localStorage');
      notify('配置中心不可用，已保存到本机', 'danger');
    }
    return savedOk;
  }

  // ----------------------------- face rendering -----------------------------

  const icon = (name, className = '') =>
    `<svg class="icon ${className}" aria-hidden="true"><use href="#i-${name}"></use></svg>`;

  function buildFace() {
    state.face = Object.hasOwn(FACES, state.face) ? state.face : 'luxe';
    const face = state.face;
    els.faceView.className = `watch-face face-${face}`;
    if (face === 'luxe') els.faceView.innerHTML = luxeFaceTemplate();
    else if (face === 'sport') els.faceView.innerHTML = sportFaceTemplate();
    else els.faceView.innerHTML = classicFaceTemplate();
    renderFaceDynamic();
  }

  function faceHeaderTemplate() {
    return `<div class="face-topline">
      <span class="face-brand">WAVE <b>·</b> ${FACES[state.face].name}</span>
      <span class="face-battery" id="faceBattery">87%</span>
    </div>
    <div class="face-date" id="faceDate">11月12日 周四</div>`;
  }

  function dockTemplate() {
    return `<div class="dock">
      <button class="dock-button" type="button" data-action="assistant">
        <span class="dock-icon">${icon('assistant')}</span><span class="dock-label">小智</span>
      </button>
      <button class="dock-button" type="button" data-action="open-music">
        <span class="dock-icon">${icon('music')}</span><span class="dock-label">音乐</span>
      </button>
      <button class="dock-button" type="button" data-action="open-apps">
        <span class="dock-icon">${icon('activity')}</span><span class="dock-label">应用</span>
      </button>
    </div>
    <button class="settings-bar" type="button" data-action="open-settings">
      ${icon('settings')}<span>设置</span>
    </button>`;
  }

  function tickMarks(size, radiusOuter, radiusInner, count = 60) {
    const center = size / 2;
    let marks = '';
    for (let i = 0; i < count; i += 1) {
      const angle = (i / count) * Math.PI * 2;
      const major = i % 5 === 0;
      const inner = major ? radiusInner - 3 : radiusInner;
      const x1 = center + Math.sin(angle) * radiusOuter;
      const y1 = center - Math.cos(angle) * radiusOuter;
      const x2 = center + Math.sin(angle) * inner;
      const y2 = center - Math.cos(angle) * inner;
      marks += `<line x1="${x1.toFixed(1)}" y1="${y1.toFixed(1)}" x2="${x2.toFixed(1)}" y2="${y2.toFixed(1)}" class="${major ? 'major' : 'minor'}"></line>`;
    }
    return marks;
  }

  function luxeFaceTemplate() {
    return `${faceHeaderTemplate()}
      <button class="aurum-dial" type="button" data-action="cycle-face" aria-label="切换表盘">
        <span class="aurum-grain"></span>
        <svg class="aurum-ticks" viewBox="0 0 300 300" aria-hidden="true">
          ${tickMarks(300, 144, 132)}
        </svg>
        <span class="aurum-sign">WAVE</span>
        <span class="aurum-sub">AUTOMATIC · SHANGHAI</span>
        <span class="aurum-day" id="faceDay">12</span>
        <span class="aurum-hand hour" id="aurumHour"></span>
        <span class="aurum-hand minute" id="aurumMinute"></span>
        <span class="aurum-hand second" id="aurumSecond"></span>
        <span class="aurum-pivot"></span>
        <span class="aurum-meta">
          <span>${icon('steps')}<b id="faceSteps">6,842</b> 步</span>
        </span>
      </button>
      ${dockTemplate()}`;
  }

  function sportFaceTemplate() {
    return `${faceHeaderTemplate()}
      <div class="flux-time" id="faceClock">09:41</div>
      <button class="flux-rings" type="button" data-action="cycle-face" aria-label="切换表盘">
        <svg viewBox="0 0 240 240" aria-hidden="true">
          <circle class="flux-track" cx="120" cy="120" r="108" stroke-width="7"></circle>
          <circle class="flux-track" cx="120" cy="120" r="92" stroke-width="7"></circle>
          <circle class="flux-track" cx="120" cy="120" r="76" stroke-width="7"></circle>
          <circle class="flux-arc steps" id="fluxStepsArc" cx="120" cy="120" r="108" stroke-width="7" stroke-linecap="round" transform="rotate(135 120 120)"></circle>
          <circle class="flux-arc battery" id="fluxBatteryArc" cx="120" cy="120" r="92" stroke-width="7" stroke-linecap="round" transform="rotate(135 120 120)"></circle>
          <circle class="flux-arc goal" id="fluxGoalArc" cx="120" cy="120" r="76" stroke-width="7" stroke-linecap="round" transform="rotate(135 120 120)"></circle>
        </svg>
        <span class="flux-center">
          <span class="flux-kicker">TODAY</span>
          <strong id="faceSteps">6,842</strong>
          <span class="flux-unit">步</span>
          <em id="faceGoal">目标 8,000 步</em>
        </span>
      </button>
      ${dockTemplate()}`;
  }

  function romanNumerals() {
    const items = [
      ['XII', 50, 11],
      ['IX', 12, 50],
    ];
    return items.map(([text, left, top]) => `<span class="roman" style="left:${left}%;top:${top}%">${text}</span>`).join('');
  }

  function classicFaceTemplate() {
    return `${faceHeaderTemplate()}
      <button class="heritage-dial" type="button" data-action="cycle-face" aria-label="切换表盘">
        <span class="heritage-texture"></span>
        <svg class="heritage-ticks" viewBox="0 0 300 300" aria-hidden="true">
          ${tickMarks(300, 144, 132)}
        </svg>
        ${romanNumerals()}
        <span class="heritage-brand">WAVE</span>
        <span class="heritage-sub">HERITAGE</span>
        <span class="heritage-day-window"><span id="faceDay">12</span></span>
        <span class="heritage-hand hour" id="heritageHour"></span>
        <span class="heritage-hand minute" id="heritageMinute"></span>
        <span class="heritage-hand second" id="heritageSecond"></span>
        <span class="heritage-pivot"></span>
        <span class="heritage-subdial"><i id="heritageSubSecond"></i></span>
        <span class="heritage-meta">${icon('steps')}<b id="faceSteps">6,842</b> 步</span>
      </button>
      ${dockTemplate()}`;
  }

  function renderFaceDynamic() {
    if (!els.faceView) return;
    const date = new Date(clock.virtualMs);
    const clockLabel = $('#faceClock', els.faceView);
    const dateLabel = $('#faceDate', els.faceView);
    const battery = $('#faceBattery', els.faceView);
    const day = $('#faceDay', els.faceView);
    const steps = $('#faceSteps', els.faceView);
    const goal = $('#faceGoal', els.faceView);

    if (clockLabel) clockLabel.textContent = formatClock(date);
    if (dateLabel) dateLabel.textContent = formatDate(date);
    if (battery) battery.textContent = `${state.charging ? '充电 ' : ''}${Math.round(state.battery)}%`;
    if (day) day.textContent = String(date.getDate()).padStart(2, '0');
    if (steps) steps.textContent = money(Math.floor(state.steps));
    if (goal) goal.textContent = `目标 ${money(state.goal)} 步`;

    const percentage = state.goal > 0 ? clamp((state.steps / state.goal) * 100, 0, 100) : 0;
    const seconds = date.getSeconds();
    const minuteAngle = (date.getMinutes() + seconds / 60) * 6;
    const hourAngle = ((date.getHours() % 12) + date.getMinutes() / 60) * 30;

    const aurumHour = $('#aurumHour', els.faceView);
    const aurumMinute = $('#aurumMinute', els.faceView);
    const aurumSecond = $('#aurumSecond', els.faceView);
    if (aurumHour) aurumHour.style.transform = `rotate(${hourAngle}deg)`;
    if (aurumMinute) aurumMinute.style.transform = `rotate(${minuteAngle}deg)`;
    if (aurumSecond) aurumSecond.style.transform = `rotate(${seconds * 6}deg)`;

    const heritageHour = $('#heritageHour', els.faceView);
    const heritageMinute = $('#heritageMinute', els.faceView);
    const heritageSecond = $('#heritageSecond', els.faceView);
    const heritageSubSecond = $('#heritageSubSecond', els.faceView);
    if (heritageHour) heritageHour.style.transform = `rotate(${hourAngle}deg)`;
    if (heritageMinute) heritageMinute.style.transform = `rotate(${minuteAngle}deg)`;
    if (heritageSecond) heritageSecond.style.transform = `rotate(${seconds * 6}deg)`;
    if (heritageSubSecond) heritageSubSecond.style.transform = `rotate(${seconds * 6}deg)`;

    const fluxSteps = $('#fluxStepsArc', els.faceView);
    const fluxBattery = $('#fluxBatteryArc', els.faceView);
    const fluxGoal = $('#fluxGoalArc', els.faceView);
    const circumference = (r) => 2 * Math.PI * r;
    if (fluxSteps) {
      const length = circumference(108) * 0.75;
      fluxSteps.setAttribute('stroke-dasharray', `${(length * percentage / 100).toFixed(1)} ${circumference(108).toFixed(1)}`);
    }
    if (fluxBattery) {
      const length = circumference(92) * 0.75;
      fluxBattery.setAttribute('stroke-dasharray', `${(length * state.battery / 100).toFixed(1)} ${circumference(92).toFixed(1)}`);
    }
    if (fluxGoal) {
      const length = circumference(76) * 0.75;
      fluxGoal.setAttribute('stroke-dasharray', `${(length * Math.min(100, percentage)).toFixed(1)} ${circumference(76).toFixed(1)}`);
    }

    els.screenDim.style.opacity = String(Math.max(0, (100 - state.brightness) / 300));
  }

  function setFace(face, options = {}) {
    if (!Object.hasOwn(FACES, face)) return;
    if (state.face === face) return;
    state.face = face;
    buildFace();
    syncControls();
    renderPageDynamic();
    persistState();
    scheduleConfigSave();
    if (!options.silent) addLog('ui', `切换表盘：${FACES[face].name}`);
  }

  function cycleFace(direction = 1) {
    const order = ['luxe', 'sport', 'classic'];
    const index = order.indexOf(state.face);
    setFace(order[(index + direction + order.length) % order.length]);
  }

  // ----------------------------- page rendering -----------------------------

  function pageHeader(title) {
    return `<button class="back-button" type="button" data-action="home" aria-label="返回">‹</button>
      <div class="page-title">${escapeHtml(title)}</div>`;
  }

  function openPage(page) {
    if (page === 'face') {
      closePage();
      return;
    }
    state.page = page;
    els.pageLayer.hidden = false;
    els.pageLayer.innerHTML = pageTemplate(page);
    $$('[data-range]', els.pageLayer).forEach(setRangeFill);
    addLog('ui', `打开${pageName(page)}`);
    syncControls();
    renderPageDynamic();
  }

  function closePage() {
    state.page = 'face';
    els.pageLayer.hidden = true;
    els.pageLayer.innerHTML = '';
    syncSideStatus();
    renderFaceDynamic();
  }

  function pageName(page) {
    return {
      settings: '设置',
      music: 'SD 卡音乐',
      activity: '今日活动',
      timer: '倒计时',
      alarm: '每日闹钟',
      apps: '应用',
      face: '表盘',
    }[page] || page;
  }

  function pageTemplate(page) {
    if (page === 'settings') return settingsTemplate();
    if (page === 'music') return musicTemplate();
    if (page === 'activity') return activityTemplate();
    if (page === 'timer') return timerTemplate();
    if (page === 'alarm') return alarmTemplate();
    if (page === 'apps') return appsTemplate();
    return '';
  }

  function settingsTemplate() {
    return `${pageHeader('手表设置')}
      <div class="setting-label label-brightness">屏幕亮度</div>
      <div class="setting-value value-brightness" id="brightnessValue">${state.brightness}%</div>
      <input class="settings-slider slider-brightness" data-range="brightness" type="range" min="10" max="100" step="1" value="${state.brightness}" aria-label="屏幕亮度">
      <div class="setting-label label-volume">语音音量</div>
      <div class="setting-value value-volume" id="volumeValue">${state.volume}%</div>
      <input class="settings-slider slider-volume" data-range="volume" type="range" min="0" max="100" step="1" value="${state.volume}" aria-label="语音音量">
      <div class="setting-label dnd-label">勿扰模式</div>
      <input class="switch dnd-switch" type="checkbox" data-switch="dnd" ${state.dnd ? 'checked' : ''} aria-label="勿扰模式">
      <div class="dnd-note">勿扰时闹钟与倒计时只显示提醒</div>
      <button class="watch-button pair-button" type="button" data-action="pair">开启网页配置配对</button>
      <div class="pair-status" id="pairStatus">手表与电脑需连接同一 Wi-Fi</div>`;
  }

  function musicTemplate() {
    const track = TRACKS[state.music.track] || TRACKS[0];
    const current = state.music.status !== 'stopped' ? state.music.track : -1;
    const rows = state.sd
      ? TRACKS.map((item, index) => `<button class="music-row ${index === current ? 'current' : ''}" type="button" data-action="music-select" data-index="${index}">
          ${escapeHtml(item.title)}<small>${escapeHtml(item.artist)}</small>
        </button>`).join('')
      : `<div class="music-empty"><b>未检测到 SD 卡</b><span>插入 SD 卡后可播放 MP3 / WAV</span><span>也可以让小智按歌名搜索</span></div>`;
    return `${pageHeader('SD 卡音乐')}
      <div class="music-status" id="musicStatus">${escapeHtml(musicStatusText())}</div>
      <div class="music-progress-track"><i id="musicBar"></i></div>
      <div class="music-progress-text" id="musicTime">00:00</div>
      <div class="music-summary" id="musicSummary">${state.sd ? `曲库 ${TRACKS.length} 首 · MP3 / WAV` : 'SD 卡未就绪'}</div>
      <div class="music-list" id="musicList">${rows}</div>
      <div class="music-buttons">
        <button class="watch-button primary" type="button" data-action="music-toggle">${state.music.status === 'playing' ? '暂停' : '播放'}</button>
        <button class="watch-button" type="button" data-action="music-prev">上一首</button>
        <button class="watch-button" type="button" data-action="music-next">下一首</button>
      </div>
      <div class="music-buttons secondary">
        <button class="watch-button" type="button" data-action="music-stop">停止播放</button>
      </div>`;
  }

  function activityTemplate() {
    return `${pageHeader('今日活动')}
      <div class="activity-wrap">
        <div class="activity-steps" id="activitySteps">${money(Math.floor(state.steps))}</div>
        <div class="activity-unit">步</div>
        <div class="activity-goal" id="activityGoal">每日目标 ${money(state.goal)} 步</div>
        <div class="activity-status" id="activityStatus">今日目标完成 ${Math.min(100, Math.round(state.steps / state.goal * 100))}%</div>
        <div class="activity-bar"><i id="activityBar"></i></div>
      </div>`;
  }

  function timerTemplate() {
    return `${pageHeader('倒计时')}
      <div class="timer-value" id="timerValue">00:00:00</div>
      <div class="timer-note" id="timerNote">选择快捷时长，或让小智帮你设置</div>
      <div class="timer-presets">
        <button class="watch-button" type="button" data-action="timer-start" data-seconds="60">1 分钟</button>
        <button class="watch-button" type="button" data-action="timer-start" data-seconds="300">5 分钟</button>
        <button class="watch-button" type="button" data-action="timer-start" data-seconds="600">10 分钟</button>
      </div>
      <div class="timer-cancel"><button class="watch-button" type="button" data-action="timer-cancel">取消计时</button></div>`;
  }

  function alarmTemplate() {
    return `${pageHeader('每日闹钟')}
      <div class="alarm-time" id="alarmTime">${formatTimeOfDay(state.alarm.hour, state.alarm.minute)}</div>
      <div class="alarm-state" id="alarmState">${state.alarm.enabled ? '闹钟已开启' : '闹钟已关闭'}</div>
      <div class="alarm-hint">选择时间 · 每日重复</div>
      <div class="alarm-presets">
        <button class="watch-button" type="button" data-action="alarm-preset" data-hour="7" data-minute="0">07:00</button>
        <button class="watch-button" type="button" data-action="alarm-preset" data-hour="7" data-minute="30">07:30</button>
        <button class="watch-button" type="button" data-action="alarm-preset" data-hour="8" data-minute="0">08:00</button>
      </div>
      <div class="alarm-toggle">
        <button class="watch-button ${state.alarm.enabled ? 'on' : ''}" type="button" data-action="alarm-toggle">${state.alarm.enabled ? '关闭闹钟' : '开启闹钟'}</button>
      </div>`;
  }

  function appsTemplate() {
    return `${pageHeader('应用')}
      <div class="app-grid">
        <button class="app-tile assistant" type="button" data-action="assistant">
          <span class="app-icon">${icon('assistant')}</span>
          <span class="app-copy"><b>小智助手</b><small>语音 · MCP</small></span>
        </button>
        <button class="app-tile music" type="button" data-action="open-music">
          <span class="app-icon">${icon('music')}</span>
          <span class="app-copy"><b>音乐播放器</b><small>SD 卡曲库</small></span>
        </button>
        <button class="app-tile activity" type="button" data-action="open-activity">
          <span class="app-icon">${icon('activity')}</span>
          <span class="app-copy"><b>活动记录</b><small>步数 · 目标</small></span>
        </button>
        <button class="app-tile alarm" type="button" data-action="open-alarm">
          <span class="app-icon">${icon('alarm')}</span>
          <span class="app-copy"><b>每日闹钟</b><small>07:00 每日</small></span>
        </button>
        <button class="app-tile timer" type="button" data-action="open-timer">
          <span class="app-icon">${icon('timer')}</span>
          <span class="app-copy"><b>倒计时</b><small>1 / 5 / 10 分钟</small></span>
        </button>
        <button class="app-tile settings" type="button" data-action="open-settings">
          <span class="app-icon">${icon('settings')}</span>
          <span class="app-copy"><b>设置</b><small>亮度 · 勿扰</small></span>
        </button>
      </div>
      <div class="app-note">内置应用可由小智语音直接控制</div>`;
  }

  function musicStatusText() {
    if (!state.sd) return '未检测到 SD 卡';
    const track = TRACKS[state.music.track];
    if (!track) return '当前没有播放音乐';
    if (state.music.status === 'playing') return `正在播放：${track.title}`;
    if (state.music.status === 'paused') return `已暂停：${track.title}`;
    return '当前没有播放音乐';
  }

  function renderPageDynamic() {
    if (!els.pageLayer || els.pageLayer.hidden) {
      renderFaceDynamic();
      return;
    }
    const page = state.page;
    if (page === 'music') {
      renderMusicPage();
    } else if (page === 'activity') {
      const steps = $('#activitySteps');
      const goal = $('#activityGoal');
      const status = $('#activityStatus');
      const bar = $('#activityBar');
      if (steps) steps.textContent = state.imu ? money(Math.floor(state.steps)) : '--';
      if (goal) goal.textContent = `每日目标 ${money(state.goal)} 步`;
      if (status) status.textContent = state.imu
        ? `今日目标完成 ${Math.min(100, Math.round(state.steps / state.goal * 100))}%`
        : '计步传感器暂不可用';
      if (bar) bar.style.width = `${state.imu ? clamp(state.steps / state.goal * 100, 0, 100) : 0}%`;
    } else if (page === 'timer') {
      const value = $('#timerValue');
      const note = $('#timerNote');
      if (value) value.textContent = state.timer.active ? formatSeconds(Math.max(0, (state.timer.endsAt - clock.virtualMs) / 1000)) : (state.timer.expired ? '00:00:00' : '00:00:00');
      if (note) note.textContent = state.timer.active ? '模拟倒计时运行中' : state.timer.expired ? '倒计时结束' : '选择快捷时长，或让小智帮你设置';
    } else if (page === 'alarm') {
      const time = $('#alarmTime');
      const stateLabel = $('#alarmState');
      const toggle = $('.alarm-toggle .watch-button', els.pageLayer);
      if (time) time.textContent = formatTimeOfDay(state.alarm.hour, state.alarm.minute);
      if (stateLabel) stateLabel.textContent = state.alarm.enabled ? '闹钟已开启' : '闹钟已关闭';
      if (toggle) {
        toggle.textContent = state.alarm.enabled ? '关闭闹钟' : '开启闹钟';
        toggle.classList.toggle('on', state.alarm.enabled);
      }
    } else if (page === 'settings') {
      const brightnessValue = $('#brightnessValue');
      const volumeValue = $('#volumeValue');
      if (brightnessValue) brightnessValue.textContent = `${state.brightness}%`;
      if (volumeValue) volumeValue.textContent = `${state.volume}%`;
      const sliderBrightness = $('[data-range="brightness"]', els.pageLayer);
      const sliderVolume = $('[data-range="volume"]', els.pageLayer);
      if (sliderBrightness && document.activeElement !== sliderBrightness) sliderBrightness.value = state.brightness;
      if (sliderVolume && document.activeElement !== sliderVolume) sliderVolume.value = state.volume;
      if (sliderBrightness) setRangeFill(sliderBrightness);
      if (sliderVolume) setRangeFill(sliderVolume);
      const dnd = $('[data-switch="dnd"]', els.pageLayer);
      if (dnd) dnd.checked = state.dnd;
      renderPairStatus();
    }
  }

  function renderMusicPage() {
    const status = $('#musicStatus');
    const bar = $('#musicBar');
    const time = $('#musicTime');
    const summary = $('#musicSummary');
    const list = $('#musicList');
    const toggle = $('[data-action="music-toggle"]', els.pageLayer);
    if (status) status.textContent = musicStatusText();
    if (summary) summary.textContent = state.sd ? `曲库 ${TRACKS.length} 首 · MP3 / WAV` : 'SD 卡未就绪';
    if (toggle) toggle.textContent = state.music.status === 'playing' ? '暂停' : '播放';
    const track = TRACKS[state.music.track];
    const duration = track?.duration || 1;
    if (bar) bar.style.width = `${clamp(state.music.position / duration * 100, 0, 100)}%`;
    if (time) {
      time.textContent = `${formatMediaTime(state.music.position)} / ${formatMediaTime(duration)}`;
    }
    if (list) {
      $$('.music-row', list).forEach((row, index) => {
        row.classList.toggle('current', state.music.status !== 'stopped' && index === state.music.track);
      });
    }
  }

  function renderPairStatus() {
    const now = performance.now();
    let text = '手表与电脑需连接同一 Wi-Fi';
    let className = 'pair-status';
    if (state.pairing.connected) {
      text = `已配对：${state.pairing.ip} · 可同步表盘、亮度、音量和勿扰`;
    } else if (state.pairing.active && now < state.pairing.expiresAt) {
      const seconds = Math.max(0, Math.ceil((state.pairing.expiresAt - now) / 1000));
      text = `${state.pairing.ip}  配对码 ${state.pairing.code}（${seconds} 秒有效）`;
      className = 'pair-status active';
    } else if (state.pairing.active || state.pairing.expired) {
      text = '配对码已过期，请重新开启';
      className = 'pair-status error';
    }
    const label = $('#pairStatus', els.pageLayer);
    if (label) {
      label.textContent = text;
      label.className = className;
    }
  }

  // ----------------------------- controls -----------------------------

  function syncControls() {
    els.batteryRange.value = state.battery;
    els.brightnessRange.value = state.brightness;
    els.volumeRange.value = state.volume;
    els.batteryOut.textContent = `${Math.round(state.battery)}%`;
    els.brightnessOut.textContent = `${state.brightness}%`;
    els.volumeOut.textContent = `${state.volume}%`;
    els.wifiToggle.checked = state.wifi;
    els.sdToggle.checked = state.sd;
    els.imuToggle.checked = state.imu;
    els.dndToggle.checked = state.dnd;
    els.autoStepsToggle.checked = state.autoSteps;
    els.soundToggle.checked = state.sound;
    els.font30Toggle.checked = state.font30;
    els.classicDarkToggle.checked = state.classicDark;
    els.hideNotifyToggle.checked = state.hideNotify;
    els.chargeToggle.textContent = state.charging ? '停止充电' : '模拟充电';
    setRangeFill(els.batteryRange);
    setRangeFill(els.brightnessRange);
    setRangeFill(els.volumeRange);
    document.body.classList.toggle('fw-font30', state.font30);
    document.body.classList.toggle('fw-classic-dark', state.classicDark);
    document.body.classList.toggle('fw-hide-notify', state.hideNotify);
    syncSideStatus();
    updateVirtualTimeDisplay();
    renderPairingSide();
    renderStateJson();
  }

  function setRangeFill(input) {
    if (!input) return;
    const min = Number(input.min || 0);
    const max = Number(input.max || 100);
    const value = Number(input.value);
    const progress = max === min ? 0 : ((value - min) / (max - min)) * 100;
    input.style.setProperty('--range-progress', `${clamp(progress, 0, 100)}%`);
  }

  function syncSideStatus() {
    els.statBattery.textContent = `${Math.round(state.battery)}%`;
    els.statSteps.textContent = state.imu ? money(Math.floor(state.steps)) : '--';
    els.statVoice.textContent = assistantLabel();
    els.statPage.textContent = pageName(state.page);
    els.gestureNote.innerHTML = state.page === 'face'
      ? '左右滑动切换表盘 · 上滑呼出小智 · 下滑查看活动<br>所有数据与状态都在本页实时模拟'
      : '点击左上角返回表盘<br>所有数据与状态都在本页实时模拟';
    els.voiceButton.textContent = state.assistant.visible ? '结束对话' : '呼出小智';
  }

  function assistantLabel() {
    if (!state.assistant.visible) return state.wifi ? '待机' : '离线';
    if (state.assistant.status === 'connecting') return '正在连接';
    if (state.assistant.status === 'speaking') return '正在回答';
    if (state.assistant.status === 'listening') return '正在聆听';
    if (state.assistant.status === 'error') return '未连接';
    return '待机';
  }

  function updateVirtualTimeDisplay() {
    const date = new Date(clock.virtualMs);
    els.virtualClock.textContent = `${formatClock(date)}:${pad(date.getSeconds())}`;
    els.virtualDate.textContent = formatDate(date);
    els.clockSpeedLabel.textContent = clock.paused ? '已暂停' : `${clock.speed}×`;
    els.pauseClock.textContent = clock.paused ? '继续时间' : '暂停时间';
    updateNextAlarm();
    if (state.page === 'face') renderFaceDynamic();
  }

  function updateNextAlarm() {
    if (!state.alarm.enabled || !state.alarm.nextAt) {
      els.nextAlarm.textContent = '闹钟未开启';
      return;
    }
    const now = new Date(clock.virtualMs);
    const next = new Date(state.alarm.nextAt);
    const sameDay = dateKey(now) === dateKey(next);
    const label = sameDay ? '今天' : '明天';
    els.nextAlarm.innerHTML = `下一次闹钟：<b>${label} ${formatTimeOfDay(next.getHours(), next.getMinutes())}</b>`;
  }

  function renderPairingSide() {
    const now = performance.now();
    if (state.pairing.connected) {
      els.pairState.textContent = '已配对';
      els.pairCountdown.textContent = '已连接';
    } else if (state.pairing.active && now < state.pairing.expiresAt) {
      els.pairState.textContent = '配对中';
      els.pairCountdown.textContent = `${Math.max(0, Math.ceil((state.pairing.expiresAt - now) / 1000))}s`;
    } else if (state.pairing.active || state.pairing.expired) {
      els.pairState.textContent = '已过期';
      els.pairCountdown.textContent = '已过期';
    } else {
      els.pairState.textContent = '未配对';
      els.pairCountdown.textContent = '--';
    }
    els.pairIp.textContent = state.pairing.ip;
    els.pairCode.textContent = state.pairing.code || '--------';
  }

  function renderStateJson() {
    if (!els.stateJson) return;
    const view = {
      face: state.face,
      page: state.page,
      brightness: state.brightness,
      volume: state.volume,
      dnd: state.dnd,
      battery: Math.round(state.battery),
      charging: state.charging,
      steps: Math.floor(state.steps),
      goal: state.goal,
      music: { track: state.music.track, status: state.music.status, position: Math.round(state.music.position) },
      timer: { active: state.timer.active, expired: state.timer.expired },
      alarm: { time: formatTimeOfDay(state.alarm.hour, state.alarm.minute), enabled: state.alarm.enabled },
      pair: state.pairing.connected ? 'connected' : state.pairing.active ? 'active' : 'idle',
      assistant: assistantLabel(),
      virtual: new Date(clock.virtualMs).toISOString(),
    };
    els.stateJson.textContent = JSON.stringify(view, null, 2);
  }

  // ----------------------------- music -----------------------------

  function currentTrack() {
    return TRACKS[state.music.track] || TRACKS[0];
  }

  function selectTrack(index, options = {}) {
    if (!state.sd) {
      notify('未检测到 SD 卡', 'danger');
      addLog('system', '音乐播放失败：SD 卡未挂载');
      return false;
    }
    const safeIndex = clamp(Number(index) || 0, 0, TRACKS.length - 1);
    state.music.track = safeIndex;
    state.music.position = 0;
    state.music.status = 'playing';
    state.music.autoPaused = false;
    addLog('mcp', `self.watch.music.play → ${currentTrack().title}`);
    renderPageDynamic();
    renderStateJson();
    persistState();
    if (!options.silent) notify(`开始播放：${currentTrack().title}`, 'system', 2600);
    return true;
  }

  function toggleMusic() {
    if (!state.sd) {
      notify('未检测到 SD 卡', 'danger');
      addLog('system', '音乐操作失败：SD 卡未挂载');
      return false;
    }
    if (state.music.status === 'playing') {
      state.music.status = 'paused';
      state.music.autoPaused = false;
      addLog('mcp', 'self.watch.music.pause → 音乐已暂停');
    } else if (state.music.status === 'paused') {
      state.music.status = 'playing';
      state.music.autoPaused = false;
      addLog('mcp', 'self.watch.music.resume → 继续播放');
    } else {
      selectTrack(state.music.track, { silent: true });
    }
    renderPageDynamic();
    renderStateJson();
    persistState();
    return true;
  }

  function nextMusic() {
    if (!state.sd) {
      notify('未检测到 SD 卡', 'danger');
      return false;
    }
    state.music.track = (state.music.track + 1) % TRACKS.length;
    state.music.position = 0;
    state.music.status = 'playing';
    state.music.autoPaused = false;
    addLog('mcp', `self.watch.music.next → ${currentTrack().title}`);
    renderPageDynamic();
    renderStateJson();
    persistState();
    return true;
  }

  function previousMusic() {
    if (!state.sd) {
      notify('未检测到 SD 卡', 'danger');
      return false;
    }
    state.music.track = (state.music.track - 1 + TRACKS.length) % TRACKS.length;
    state.music.position = 0;
    state.music.status = 'playing';
    state.music.autoPaused = false;
    addLog('mcp', `self.watch.music.previous → ${currentTrack().title}`);
    renderPageDynamic();
    renderStateJson();
    persistState();
    return true;
  }

  function stopMusic() {
    state.music.status = 'stopped';
    state.music.position = 0;
    state.music.autoPaused = false;
    addLog('mcp', 'self.watch.music.stop → 音乐已停止');
    renderPageDynamic();
    renderStateJson();
    persistState();
    return true;
  }

  function pauseMusicForAssistant() {
    if (state.music.status === 'playing') {
      state.music.status = 'paused';
      state.music.autoPaused = true;
      addLog('system', '小智开启对话，音乐已暂停');
      renderPageDynamic();
    }
  }

  function resumeMusicAfterAssistant() {
    if (state.music.autoPaused) {
      state.music.status = 'playing';
      state.music.autoPaused = false;
      addLog('system', '对话结束，音乐继续播放');
      renderPageDynamic();
    }
  }

  function tickMusic(realDelta) {
    if (state.music.status !== 'playing') return;
    state.music.position += realDelta / 1000;
    const duration = currentTrack().duration;
    if (state.music.position >= duration) {
      state.music.track = (state.music.track + 1) % TRACKS.length;
      state.music.position = 0;
      addLog('system', `自动播放下一首：${currentTrack().title}`);
      renderPageDynamic();
    }
    if (state.page === 'music') renderMusicPage();
  }

  // ----------------------------- alarm + timer -----------------------------

  function computeNextAlarm() {
    if (!state.alarm.enabled) {
      state.alarm.nextAt = 0;
      return;
    }
    const now = new Date(clock.virtualMs);
    const next = new Date(now);
    next.setHours(state.alarm.hour, state.alarm.minute, 0, 0);
    if (next.getTime() <= now.getTime()) next.setDate(next.getDate() + 1);
    state.alarm.nextAt = next.getTime();
  }

  function setAlarm(hour, minute, enabled = true, silent = false) {
    state.alarm.hour = clamp(Number(hour) || 0, 0, 23);
    state.alarm.minute = clamp(Number(minute) || 0, 0, 59);
    state.alarm.enabled = enabled;
    computeNextAlarm();
    renderPageDynamic();
    updateNextAlarm();
    persistState();
    if (!silent) {
      addLog('mcp', `self.watch.alarm.set → ${formatTimeOfDay(state.alarm.hour, state.alarm.minute)} ${enabled ? '已开启' : '已关闭'}`);
    }
  }

  function toggleAlarm() {
    state.alarm.enabled = !state.alarm.enabled;
    computeNextAlarm();
    renderPageDynamic();
    updateNextAlarm();
    persistState();
    addLog('mcp', `self.watch.alarm.${state.alarm.enabled ? 'set' : 'disable'} → ${state.alarm.enabled ? '已开启' : '已关闭'}`);
  }

  function checkAlarm() {
    if (!state.alarm.enabled || !state.alarm.nextAt) return;
    if (clock.virtualMs < state.alarm.nextAt) return;
    let fired = 0;
    while (state.alarm.enabled && state.alarm.nextAt && clock.virtualMs >= state.alarm.nextAt) {
      const fireAt = new Date(state.alarm.nextAt);
      state.alarm.nextAt += 24 * 60 * 60 * 1000;
      fired += 1;
      if (fired > 10) break;
      const label = `${formatTimeOfDay(fireAt.getHours(), fireAt.getMinutes())} 每日闹钟`;
      addLog('alarm', `${label}${state.dnd ? ' · 勿扰：仅提醒' : ''}`);
      notify(`${label}${state.dnd ? '（勿扰）' : ''}`, 'alarm', 7600);
      if (state.dnd) addLog('system', '勿扰模式：闹钟不播放提示音');
    }
    updateNextAlarm();
  }

  function startTimer(seconds) {
    const duration = clamp(Number(seconds) || 0, 1, 86400);
    state.timer.active = true;
    state.timer.expired = false;
    state.timer.duration = duration;
    state.timer.endsAt = clock.virtualMs + duration * 1000;
    addLog('mcp', `self.watch.timer.start → ${duration} 秒`);
    renderPageDynamic();
    persistState();
    notify(`倒计时已开始：${Math.round(duration)} 秒`, 'system', 3000);
  }

  function cancelTimer() {
    if (!state.timer.active) {
      notify('当前没有运行中的倒计时', 'system', 2800);
      return;
    }
    state.timer.active = false;
    state.timer.expired = false;
    state.timer.endsAt = 0;
    addLog('mcp', 'self.watch.timer.cancel → 倒计时已取消');
    renderPageDynamic();
  }

  function checkTimer() {
    if (!state.timer.active) return;
    if (clock.virtualMs < state.timer.endsAt) return;
    state.timer.active = false;
    state.timer.expired = true;
    addLog('timer', `倒计时结束${state.dnd ? ' · 勿扰：仅提醒' : ''}`);
    notify(`倒计时结束${state.dnd ? '（勿扰）' : ''}`, 'timer', 7000);
    renderPageDynamic();
  }

  // ----------------------------- assistant -----------------------------

  function setAssistantState(status, caption) {
    state.assistant.status = status;
    state.assistant.caption = caption;
    renderAssistant();
    syncSideStatus();
    renderStateJson();
  }

  function renderAssistant() {
    requestAnimationFrame(() => {
      els.assistantOverlay.hidden = !state.assistant.visible;
      els.assistantOverlay.classList.toggle('listening', state.assistant.status === 'listening');
      els.assistantOverlay.classList.toggle('speaking', state.assistant.status === 'speaking');
      if (state.assistant.status === 'connecting') {
        els.assistantStatus.textContent = '正在连接小智';
        els.assistantText.textContent = '正在连接小智';
        els.assistantCaption.textContent = '请稍候，我正在连接语音服务';
      } else if (state.assistant.status === 'listening') {
        els.assistantStatus.textContent = '正在聆听';
        els.assistantText.textContent = '正在聆听';
        els.assistantCaption.textContent = state.assistant.caption || '可以直接说话，也可以随时打断';
      } else if (state.assistant.status === 'speaking') {
        els.assistantStatus.textContent = '小智正在回答';
        els.assistantText.textContent = '小智正在回答';
        els.assistantCaption.textContent = state.assistant.caption || '已收到';
      } else if (state.assistant.status === 'error') {
        els.assistantStatus.textContent = '未连接';
        els.assistantText.textContent = '网络未连接';
        els.assistantCaption.textContent = state.assistant.caption || '请检查 Wi-Fi 后重试';
      } else {
        els.assistantStatus.textContent = '待机';
        els.assistantText.textContent = '你好，我在这里';
        els.assistantCaption.textContent = '可以直接说话，也可以随时打断';
      }
    });
  }

  function startAssistant() {
    clearTimeout(assistantIdleTimer);
    clearTimeout(assistantCloseTimer);
    if (state.assistant.visible) {
      closeAssistant();
      return;
    }
    pauseMusicForAssistant();
    state.assistant.visible = true;
    state.assistant.status = 'connecting';
    state.assistant.caption = '';
    renderAssistant();
    syncSideStatus();
    addLog('voice', '小智被唤醒，正在连接语音服务');
    assistantIdleTimer = setTimeout(() => {
      if (!state.assistant.visible) return;
      if (!state.wifi) {
        setAssistantState('error', '网络未连接，无法使用小智');
        addLog('voice', '语音连接失败：Wi-Fi 未连接');
        notify('Wi-Fi 未连接，无法连接小智', 'danger');
        assistantCloseTimer = setTimeout(closeAssistant, 2200);
        return;
      }
      setAssistantState('listening', '可以直接说话，也可以随时打断');
      addLog('voice', '小智进入聆听状态');
      assistantIdleTimer = setTimeout(() => {
        if (state.assistant.visible && state.assistant.status === 'listening') {
          addLog('voice', '长时间未检测到指令，退出聆听');
          closeAssistant();
        }
      }, 18000);
    }, 720);
  }

  function speak(text, duration = 3200) {
    if (!state.wifi) {
      notify('Wi-Fi 未连接，无法连接小智', 'danger');
      addLog('voice', `语音不可用：${text}`);
      return;
    }
    clearTimeout(assistantIdleTimer);
    clearTimeout(assistantCloseTimer);
    pauseMusicForAssistant();
    state.assistant.visible = true;
    setAssistantState('speaking', text);
    addLog('voice', text);
    assistantCloseTimer = setTimeout(closeAssistant, duration);
  }

  function closeAssistant() {
    clearTimeout(assistantIdleTimer);
    clearTimeout(assistantCloseTimer);
    if (!state.assistant.visible) return;
    state.assistant.visible = false;
    state.assistant.status = 'idle';
    state.assistant.caption = '';
    renderAssistant();
    resumeMusicAfterAssistant();
    syncSideStatus();
    addLog('voice', '小智结束对话');
  }

  // ----------------------------- command parser -----------------------------

  function findTrack(query) {
    const needle = String(query || '').toLowerCase().replace(/\s+/g, '');
    if (!needle) return 0;
    const exact = TRACKS.findIndex((track) => `${track.artist}${track.title}`.toLowerCase().includes(needle)
      || track.title.toLowerCase().includes(needle));
    if (exact >= 0) return exact;
    const partial = TRACKS.findIndex((track) => needle.split('').every((char) => (track.title + track.artist).includes(char)));
    return partial >= 0 ? partial : -1;
  }

  function commandResult(response, tool, changesPage = false) {
    addLog('mcp', `${tool} → ${response}`);
    els.lastResponse.innerHTML = `<b>${escapeHtml(tool)}</b><br>${escapeHtml(response)}`;
    speak(response);
    if (changesPage) {
      // Page changes stay behind the assistant overlay; it becomes visible after the reply.
      syncSideStatus();
    }
  }

  function runCommand(raw) {
    const text = String(raw || '').trim();
    if (!text) return;
    els.commandInput.value = '';
    addLog('voice', `用户：${text}`);
    const normalized = text.replace(/[，。！？、,.!?]/g, ' ').replace(/\s+/g, ' ').trim();
    let match;

    if (/(^|\s)(停止|关闭|关掉)音乐/.test(normalized)) {
      stopMusic();
      commandResult('音乐已停止。', 'self.watch.music.stop');
      return;
    }
    if (/(^|\s)(暂停|暂停播放)音乐?/.test(normalized)) {
      const ok = toggleMusic();
      commandResult(ok ? (state.music.status === 'paused' ? `已暂停：${currentTrack().title}` : '当前没有播放中的音乐。') : '未检测到 SD 卡，无法暂停音乐。', 'self.watch.music.pause');
      return;
    }
    if (/(^|\s)(继续播放|继续|恢复播放|恢复)音乐?/.test(normalized)) {
      if (state.music.status === 'paused') {
        state.music.status = 'playing';
        state.music.autoPaused = false;
        addLog('mcp', 'self.watch.music.resume → 继续播放');
      } else {
        selectTrack(state.music.track, { silent: true });
      }
      commandResult(`继续播放：${currentTrack().title}`, 'self.watch.music.resume');
      return;
    }
    if (/(下一首|下一曲|切歌|换一首)/.test(normalized)) {
      if (nextMusic()) commandResult(`下一首：${currentTrack().title}`, 'self.watch.music.next');
      else commandResult('未检测到 SD 卡，无法切换音乐。', 'self.watch.music.next');
      return;
    }
    if (/(上一首|上一曲)/.test(normalized)) {
      if (previousMusic()) commandResult(`上一首：${currentTrack().title}`, 'self.watch.music.previous');
      else commandResult('未检测到 SD 卡，无法切换音乐。', 'self.watch.music.previous');
      return;
    }
    if ((match = normalized.match(/(?:播放|放|来一首|听|找)(.*)/))) {
      const query = match[1].replace(/音乐|歌曲|首歌/g, '').trim();
      const index = findTrack(query);
      if (index < 0) {
        commandResult('SD 卡里没有找到匹配的音乐。', 'self.watch.music.search');
      } else if (!state.sd) {
        commandResult('未检测到 SD 卡，无法播放音乐。', 'self.watch.music.play');
      } else {
        selectTrack(index, { silent: true });
        commandResult(`开始播放：${currentTrack().title}`, 'self.watch.music.play');
      }
      return;
    }
    if (/(亮度)/.test(normalized)) {
      const valueMatch = normalized.match(/(\d{1,3})/);
      if (valueMatch) {
        const value = clamp(Number(valueMatch[1]), 10, 100);
        state.brightness = value;
        syncControls();
        renderPageDynamic();
        persistState();
        scheduleConfigSave();
        commandResult(`屏幕亮度已设为 ${value}%。`, 'self.watch.settings.set_brightness');
      } else {
        commandResult(`当前屏幕亮度是 ${state.brightness}%。`, 'self.watch.settings.status');
      }
      return;
    }
    if (/(音量)/.test(normalized)) {
      const valueMatch = normalized.match(/(\d{1,3})/);
      if (valueMatch) {
        const value = clamp(Number(valueMatch[1]), 0, 100);
        state.volume = value;
        syncControls();
        renderPageDynamic();
        persistState();
        scheduleConfigSave();
        commandResult(`语音音量已设为 ${value}%。`, 'self.watch.settings.set_volume');
      } else {
        commandResult(`当前语音音量是 ${state.volume}%。`, 'self.watch.settings.status');
      }
      return;
    }
    if (/(勿扰|免打扰)/.test(normalized)) {
      const enabled = !/(关闭|关掉|取消|关)/.test(normalized);
      state.dnd = enabled;
      syncControls();
      renderPageDynamic();
      persistState();
      scheduleConfigSave();
      commandResult(enabled ? '勿扰模式已开启。' : '勿扰模式已关闭。', 'self.watch.settings.set_dnd');
      return;
    }
    if ((match = normalized.match(/(?:打开|进入)\s*(音乐|活动|闹钟|每日闹钟|倒计时|计时器|设置|应用|小智)/))) {
      const target = match[1];
      if (target === '小智') {
        commandResult('小智已经在听你说。', 'self.watch.app.open');
      } else {
        const page = target === '音乐' ? 'music'
          : target === '活动' ? 'activity'
          : target.includes('闹钟') ? 'alarm'
          : target.includes('倒计时') || target.includes('计时器') ? 'timer'
          : target === '设置' ? 'settings' : 'apps';
        openPage(page);
        commandResult(`已打开${pageName(page)}。`, 'self.watch.app.open', true);
      }
      return;
    }
    if (/(闹钟)/.test(normalized)) {
      if (/(关闭|关掉|取消|关)/.test(normalized)) {
        state.alarm.enabled = false;
        computeNextAlarm();
        renderPageDynamic();
        updateNextAlarm();
        persistState();
        commandResult('每日闹钟已关闭。', 'self.watch.alarm.disable');
        return;
      }
      const timeMatch = normalized.match(/(\d{1,2})\s*[:：点]\s*(\d{1,2})?/);
      if (timeMatch) {
        const hour = clamp(Number(timeMatch[1]), 0, 23);
        const minute = clamp(Number(timeMatch[2] || 0), 0, 59);
        setAlarm(hour, minute, true, true);
        commandResult(`已设置每日闹钟 ${formatTimeOfDay(hour, minute)}。`, 'self.watch.alarm.set');
      } else {
        commandResult(`当前每日闹钟：${formatTimeOfDay(state.alarm.hour, state.alarm.minute)}，${state.alarm.enabled ? '已开启' : '已关闭'}。`, 'self.watch.alarm.status');
      }
      return;
    }
    if (/(倒计时|计时器|计时)/.test(normalized)) {
      if (/(取消|停止|关掉|关闭)/.test(normalized)) {
        cancelTimer();
        commandResult('倒计时已取消。', 'self.watch.timer.cancel');
        return;
      }
      const durationMatch = normalized.match(/(\d+(?:\.\d+)?)\s*(小时|钟头|分钟|分|秒)/);
      if (durationMatch) {
        const value = Number(durationMatch[1]);
        const unit = durationMatch[2];
        const seconds = Math.round(value * (unit === '秒' ? 1 : unit === '分' || unit === '分钟' ? 60 : 3600));
        startTimer(seconds);
        commandResult(`已开始倒计时 ${seconds >= 60 ? `${Math.round(seconds / 60)} 分钟` : `${seconds} 秒`}。`, 'self.watch.timer.start');
      } else {
        commandResult('请告诉我倒计时的时长，例如“倒计时 5 分钟”。', 'self.watch.timer.start');
      }
      return;
    }
    if (/(鎏金|活力|运动|经典|表盘)/.test(normalized) && !/(打开|应用|设置)/.test(normalized)) {
      let face = null;
      if (/鎏金/.test(normalized)) face = 'luxe';
      else if (/活力|运动/.test(normalized)) face = 'sport';
      else if (/经典/.test(normalized)) face = 'classic';
      if (face) {
        setFace(face);
        commandResult(`已切换到${FACES[face].name}表盘。`, 'self.watch.face.set');
      } else {
        commandResult(`当前使用的是${FACES[state.face].name}表盘。`, 'self.watch.face.status');
      }
      return;
    }
    if (/(几点|时间|现在)/.test(normalized)) {
      const date = new Date(clock.virtualMs);
      commandResult(`现在是 ${formatClock(date)}，${formatDate(date)}。`, 'self.watch.system.status');
      return;
    }
    if (/(电量|电池)/.test(normalized)) {
      commandResult(`当前电量 ${Math.round(state.battery)}%${state.charging ? '，正在充电' : ''}。`, 'self.watch.system.status');
      return;
    }
    if (/(步数|走了多少|活动)/.test(normalized) && !/(目标)/.test(normalized)) {
      commandResult(state.imu ? `今天已走 ${money(Math.floor(state.steps))} 步，每日目标 ${money(state.goal)} 步。` : '计步传感器暂不可用，请检查 IMU。', 'self.watch.activity.get_steps');
      return;
    }
    if ((match = normalized.match(/(?:目标|步数目标)[^\d]*(\d{3,5})/))) {
      const goal = clamp(Number(match[1]), 1000, 50000);
      state.goal = goal;
      renderFaceDynamic();
      renderPageDynamic();
      persistState();
      commandResult(`每日步数目标已设为 ${money(goal)} 步。`, 'self.watch.activity.set_goal');
      return;
    }

    els.lastResponse.innerHTML = `<b>未识别</b><br>${escapeHtml(text)}`;
    speak('这个指令我还没学会，可以试试播放音乐、设置闹钟或倒计时。');
    addLog('voice', `未识别指令：${text}`);
  }

  // ----------------------------- pairing -----------------------------

  function startPairing() {
    if (!state.wifi) {
      notify('请先连接 Wi-Fi', 'danger');
      addLog('system', '配对失败：Wi-Fi 未连接');
      return;
    }
    state.pairing.active = true;
    state.pairing.connected = false;
    state.pairing.expired = false;
    state.pairing.code = String(Math.floor(Math.random() * 100000000)).padStart(8, '0');
    state.pairing.expiresAt = performance.now() + 60_000;
    renderPairingSide();
    renderPairStatus();
    addLog('config', `开启网页配置配对：${state.pairing.ip} / ${state.pairing.code}`);
    notify('配对码已生成，60 秒内有效', 'system', 4000);
  }

  function simulatePairing() {
    const code = els.pairInput.value.trim();
    if (!state.pairing.active) {
      notify('请先在手表设置中开启配对', 'danger');
      addLog('system', '模拟配对失败：未开启配对');
      return;
    }
    if (performance.now() >= state.pairing.expiresAt) {
      state.pairing.active = false;
      state.pairing.expired = true;
      renderPairingSide();
      renderPairStatus();
      notify('配对码已过期，请重新开启', 'danger');
      return;
    }
    if (code !== state.pairing.code) {
      notify('配对码不正确', 'danger');
      addLog('system', '模拟配对失败：配对码不正确');
      return;
    }
    state.pairing.active = false;
    state.pairing.connected = true;
    state.pairing.expired = false;
    els.pairInput.value = '';
    renderPairingSide();
    renderPairStatus();
    addLog('config', `模拟配对成功：${state.pairing.ip}`);
    notify('配对成功，可以去配置中心同步设置', 'system', 4200);
  }

  // ----------------------------- ticking -----------------------------

  function tick(now) {
    const realDelta = clamp(now - lastFrame, 0, 500);
    lastFrame = now;
    if (!clock.paused) {
      clock.virtualMs += realDelta * clock.speed;
    }

    const date = new Date(clock.virtualMs);
    const key = dateKey(date);
    if (key !== clock.lastDateKey) {
      clock.lastDateKey = key;
      state.steps = 0;
      state.timer.expired = false;
      computeNextAlarm();
      addLog('system', '跨天：步数已归零');
      renderFaceDynamic();
      renderPageDynamic();
      persistState();
    }

    if (state.autoSteps && state.imu && !clock.paused) {
      state.steps += realDelta / 1000 * 1.1;
      if (Math.random() < 0.002) renderFaceDynamic();
    }

    tickMusic(realDelta);
    checkTimer();
    checkAlarm();
    checkPairing();
    checkLowBattery();

    if (now % 500 < 20) {
      updateVirtualTimeDisplay();
      syncSideStatus();
      renderFaceDynamic();
      renderPageDynamic();
      renderStateJson();
    }

    requestAnimationFrame(tick);
  }

  function checkPairing() {
    if (!state.pairing.active) return;
    if (performance.now() < state.pairing.expiresAt) return;
    state.pairing.active = false;
    state.pairing.expired = true;
    renderPairingSide();
    renderPairStatus();
    addLog('config', '配对码已过期');
  }

  function checkLowBattery() {
    if (state.battery <= LOW_BATTERY && !lowBatteryAlerted && !state.charging) {
      lowBatteryAlerted = true;
      notify('电量低，请为手表充电', 'danger', 7000);
      addLog('system', `电量低：${Math.round(state.battery)}%`);
    } else if (state.battery > LOW_BATTERY + 5) {
      lowBatteryAlerted = false;
    }
  }

  function setBattery(value) {
    state.battery = clamp(Number(value) || 0, 0, 100);
    els.batteryRange.value = state.battery;
    els.batteryOut.textContent = `${Math.round(state.battery)}%`;
    setRangeFill(els.batteryRange);
    checkLowBattery();
    renderFaceDynamic();
    renderStateJson();
    persistState();
  }

  // ----------------------------- fit -----------------------------

  function fitDevice() {
    if (!els.stage) return;
    const width = els.stage.clientWidth - 42;
    const height = els.stage.clientHeight - 150;
    const scale = clamp(Math.min(1, width / 470, height / 640), 0.52, 1);
    els.deviceWrap.style.width = `${470 * scale}px`;
    els.deviceWrap.style.height = `${640 * scale}px`;
    els.device.style.transform = `scale(${scale})`;
  }

  // ----------------------------- events -----------------------------

  function bindEvents() {
    els.screenInner.addEventListener('click', (event) => {
      const target = event.target.closest('[data-action]');
      if (!target) return;
      const action = target.dataset.action;
      if (action === 'home') closePage();
      else if (action === 'assistant') startAssistant();
      else if (action === 'cycle-face') cycleFace(1);
      else if (action === 'open-music') openPage('music');
      else if (action === 'open-apps') openPage('apps');
      else if (action === 'open-settings') openPage('settings');
      else if (action === 'open-activity') openPage('activity');
      else if (action === 'open-timer') openPage('timer');
      else if (action === 'open-alarm') openPage('alarm');
      else if (action === 'pair') startPairing();
      else if (action === 'music-toggle') {
        toggleMusic();
        pushMusicPageRefresh();
      } else if (action === 'music-next') {
        nextMusic();
        pushMusicPageRefresh();
      } else if (action === 'music-prev') {
        previousMusic();
        pushMusicPageRefresh();
      } else if (action === 'music-stop') {
        stopMusic();
        pushMusicPageRefresh();
      } else if (action === 'music-select') {
        selectTrack(Number(target.dataset.index));
        pushMusicPageRefresh();
      } else if (action === 'timer-start') {
        startTimer(Number(target.dataset.seconds));
      } else if (action === 'timer-cancel') {
        cancelTimer();
      } else if (action === 'alarm-preset') {
        setAlarm(Number(target.dataset.hour), Number(target.dataset.minute), true);
      } else if (action === 'alarm-toggle') {
        toggleAlarm();
      }
    });

    els.screenInner.addEventListener('input', (event) => {
      const input = event.target.closest('[data-range]');
      if (!input) return;
      const key = input.dataset.range;
      const value = Number(input.value);
      if (key === 'brightness') {
        state.brightness = clamp(value, 10, 100);
        els.brightnessRange.value = state.brightness;
        els.brightnessOut.textContent = `${state.brightness}%`;
        setRangeFill(els.brightnessRange);
        renderPageDynamic();
        renderFaceDynamic();
      } else if (key === 'volume') {
        state.volume = clamp(value, 0, 100);
        els.volumeRange.value = state.volume;
        els.volumeOut.textContent = `${state.volume}%`;
        setRangeFill(els.volumeRange);
        renderPageDynamic();
      }
      persistState();
      scheduleConfigSave();
    });

    els.screenInner.addEventListener('change', (event) => {
      const input = event.target.closest('[data-switch]');
      if (!input) return;
      if (input.dataset.switch === 'dnd') {
        state.dnd = input.checked;
        els.dndToggle.checked = state.dnd;
        addLog('ui', `勿扰模式${state.dnd ? '开启' : '关闭'}`);
        persistState();
        scheduleConfigSave();
        renderPageDynamic();
      }
    });

    els.assistantEnd.addEventListener('click', () => closeAssistant());

    els.prevFace.addEventListener('click', () => cycleFace(-1));
    els.nextFace.addEventListener('click', () => cycleFace(1));
    els.voiceButton.addEventListener('click', () => {
      if (state.assistant.visible) closeAssistant();
      else startAssistant();
    });

    let gestureStart = null;
    els.screen.addEventListener('pointerdown', (event) => {
      if (state.page !== 'face' || state.assistant.visible) return;
      gestureStart = { x: event.clientX, y: event.clientY };
    });
    els.screen.addEventListener('pointerup', (event) => {
      if (!gestureStart) return;
      const dx = event.clientX - gestureStart.x;
      const dy = event.clientY - gestureStart.y;
      gestureStart = null;
      if (Math.max(Math.abs(dx), Math.abs(dy)) < 38) return;
      if (Math.abs(dx) > Math.abs(dy)) cycleFace(dx < 0 ? 1 : -1);
      else if (dy < 0) startAssistant();
      else openPage('activity');
    });
    els.screen.addEventListener('pointercancel', () => { gestureStart = null; });

    els.batteryRange.addEventListener('input', () => setBattery(els.batteryRange.value));
    els.brightnessRange.addEventListener('input', () => {
      state.brightness = clamp(Number(els.brightnessRange.value), 10, 100);
      els.brightnessOut.textContent = `${state.brightness}%`;
      setRangeFill(els.brightnessRange);
      renderFaceDynamic();
      renderPageDynamic();
      persistState();
      scheduleConfigSave();
    });
    els.volumeRange.addEventListener('input', () => {
      state.volume = clamp(Number(els.volumeRange.value), 0, 100);
      els.volumeOut.textContent = `${state.volume}%`;
      setRangeFill(els.volumeRange);
      renderPageDynamic();
      persistState();
      scheduleConfigSave();
    });

    els.wifiToggle.addEventListener('change', () => {
      state.wifi = els.wifiToggle.checked;
      if (!state.wifi) {
        if (state.assistant.visible) closeAssistant();
        if (state.pairing.active) {
          state.pairing.active = false;
          renderPairingSide();
          renderPairStatus();
        }
        addLog('system', 'Wi-Fi 已断开');
        notify('Wi-Fi 已断开', 'danger');
      } else {
        addLog('system', 'Wi-Fi 已连接');
        notify('Wi-Fi 已连接', 'system', 2600);
      }
      persistState();
      syncSideStatus();
    });
    els.sdToggle.addEventListener('change', () => {
      state.sd = els.sdToggle.checked;
      if (!state.sd) {
        stopMusic();
        addLog('system', 'SD 卡已移除');
        notify('SD 卡已移除', 'danger');
      } else {
        addLog('system', 'SD 卡已插入');
        notify('SD 卡已插入', 'system', 2600);
      }
      if (state.page === 'music') {
        els.pageLayer.innerHTML = musicTemplate();
        $$('[data-range]', els.pageLayer).forEach(setRangeFill);
      }
      renderPageDynamic();
      persistState();
    });
    els.imuToggle.addEventListener('change', () => {
      state.imu = els.imuToggle.checked;
      addLog('system', `IMU 计步${state.imu ? '已启用' : '不可用'}`);
      renderPageDynamic();
      persistState();
    });
    els.dndToggle.addEventListener('change', () => {
      state.dnd = els.dndToggle.checked;
      addLog('ui', `勿扰模式${state.dnd ? '开启' : '关闭'}`);
      renderPageDynamic();
      persistState();
      scheduleConfigSave();
    });
    els.autoStepsToggle.addEventListener('change', () => {
      state.autoSteps = els.autoStepsToggle.checked;
      addLog('system', `自动计步${state.autoSteps ? '已开启' : '已关闭'}`);
      persistState();
    });
    els.soundToggle.addEventListener('change', () => {
      state.sound = els.soundToggle.checked;
      persistState();
      if (state.sound) playTone(660, (ensureAudio()?.currentTime || 0) + 0.02, 0.1, 0.02);
    });

    els.chargeToggle.addEventListener('click', () => {
      state.charging = !state.charging;
      addLog('system', state.charging ? '开始模拟充电' : '停止充电');
      syncControls();
      renderFaceDynamic();
      persistState();
    });
    els.stepAdd.addEventListener('click', () => {
      state.steps += 500;
      addLog('ui', '手动增加 500 步');
      renderFaceDynamic();
      renderPageDynamic();
      persistState();
    });
    els.batteryDrop.addEventListener('click', () => {
      setBattery(state.battery - 20);
      addLog('ui', '电量 -20%');
    });

    $$('#speedGroup button').forEach((button) => {
      button.addEventListener('click', () => {
        clock.speed = Number(button.dataset.speed);
        $$('#speedGroup button').forEach((item) => item.classList.toggle('active', item === button));
        updateVirtualTimeDisplay();
        addLog('system', `虚拟时间速度：${clock.speed}×`);
      });
    });
    els.pauseClock.addEventListener('click', () => {
      clock.paused = !clock.paused;
      updateVirtualTimeDisplay();
      addLog('system', clock.paused ? '虚拟时间已暂停' : '虚拟时间继续');
    });
    els.jumpHour.addEventListener('click', () => {
      clock.virtualMs += 3600 * 1000;
      renderFaceDynamic();
      updateVirtualTimeDisplay();
      addLog('system', '虚拟时间快进 1 小时');
    });
    els.resetClock.addEventListener('click', () => {
      clock.virtualMs = Date.now();
      clock.speed = 1;
      clock.paused = false;
      clock.lastDateKey = dateKey(new Date(clock.virtualMs));
      $$('#speedGroup button').forEach((item) => item.classList.toggle('active', item.dataset.speed === '1'));
      computeNextAlarm();
      updateVirtualTimeDisplay();
      renderFaceDynamic();
      addLog('system', '虚拟时间回到现在');
    });
    els.applyTime.addEventListener('click', () => {
      const [hour, minute, second] = els.jumpTime.value.split(':').map(Number);
      if (!Number.isFinite(hour)) {
        notify('请选择要跳转的时间', 'danger');
        return;
      }
      const date = new Date(clock.virtualMs);
      date.setHours(clamp(hour, 0, 23), clamp(minute || 0, 0, 59), clamp(second || 0, 0, 59), 0);
      clock.virtualMs = date.getTime();
      clock.lastDateKey = dateKey(date);
      computeNextAlarm();
      updateVirtualTimeDisplay();
      renderFaceDynamic();
      addLog('system', `虚拟时间跳转到 ${pad(hour)}:${pad(minute || 0)}`);
    });

    els.runCommand.addEventListener('click', () => runCommand(els.commandInput.value));
    els.commandInput.addEventListener('keydown', (event) => {
      if (event.key === 'Enter') runCommand(els.commandInput.value);
    });
    $$('#commandChips button').forEach((button) => {
      button.addEventListener('click', () => runCommand(button.dataset.command));
    });

    els.openPairing.addEventListener('click', () => {
      if (state.page !== 'settings') openPage('settings');
      startPairing();
    });
    els.copyPair.addEventListener('click', async () => {
      const text = state.pairing.code
        ? `${state.pairing.ip} / ${state.pairing.code}`
        : '请先开启配对';
      try {
        await navigator.clipboard.writeText(text);
        notify('配对信息已复制', 'system', 2400);
      } catch {
        notify(text, 'system', 4000);
      }
      addLog('config', `复制配对信息：${text}`);
    });
    els.simulatePair.addEventListener('click', simulatePairing);
    els.syncConfig.addEventListener('click', async () => {
      const ok = await loadSharedConfig(false);
      notify(ok ? '已从配置中心读取' : '配置中心不可用', ok ? 'system' : 'danger');
    });
    els.pushConfig.addEventListener('click', pushConfigToPortal);

    els.font30Toggle.addEventListener('change', () => {
      state.font30 = els.font30Toggle.checked;
      document.body.classList.toggle('fw-font30', state.font30);
      addLog('system', `复刻 30px 字体：${state.font30 ? '开启' : '关闭'}`);
      persistState();
    });
    els.classicDarkToggle.addEventListener('change', () => {
      state.classicDark = els.classicDarkToggle.checked;
      document.body.classList.toggle('fw-classic-dark', state.classicDark);
      addLog('system', `经典表盘深色底：${state.classicDark ? '开启' : '关闭'}`);
      persistState();
    });
    els.hideNotifyToggle.addEventListener('change', () => {
      state.hideNotify = els.hideNotifyToggle.checked;
      document.body.classList.toggle('fw-hide-notify', state.hideNotify);
      addLog('system', `系统通知遮挡：${state.hideNotify ? '开启' : '关闭'}`);
      persistState();
    });
    els.copyState.addEventListener('click', async () => {
      try {
        await navigator.clipboard.writeText(els.stateJson.textContent || '');
        notify('状态 JSON 已复制', 'system', 2400);
      } catch {
        notify('复制失败，请手动选择文本', 'danger');
      }
    });
    els.clearLog.addEventListener('click', () => {
      state.log = [];
      renderLog();
    });

    window.addEventListener('resize', fitDevice);
    if (typeof ResizeObserver !== 'undefined') {
      new ResizeObserver(fitDevice).observe(els.stage);
    }
  }

  function pushMusicPageRefresh() {
    if (state.page === 'music') {
      // Rebuild only when the selected row / SD state changes, so the list stays in sync.
      renderMusicPage();
      const rows = $$('.music-row', els.pageLayer);
      rows.forEach((row, index) => row.classList.toggle('current', state.music.status !== 'stopped' && index === state.music.track));
      const toggle = $('[data-action="music-toggle"]', els.pageLayer);
      if (toggle) toggle.textContent = state.music.status === 'playing' ? '暂停' : '播放';
    }
    renderStateJson();
  }

  // ----------------------------- boot -----------------------------

  function init() {
    if (!Object.hasOwn(FACES, state.face)) state.face = 'luxe';
    state.page = 'face';
    computeNextAlarm();
    buildFace();
    syncControls();
    renderAssistant();
    setConfigSource('local');
    addLog('system', 'WAVE Lab 启动：410 × 502 高保真模拟器');
    addLog('system', '默认按设计稿渲染；可在「固件缺陷复刻」查看已知问题');
    bindEvents();
    els.jumpTime.value = `${pad(new Date(clock.virtualMs).getHours())}:${pad(new Date(clock.virtualMs).getMinutes())}:00`;
    els.assistantOverlay.hidden = true;
    els.pageLayer.hidden = true;
    els.toast.hidden = true;
    fitDevice();
    loadSharedConfig(true);
    requestAnimationFrame(tick);

    window.waveLab = {
      state,
      clock,
      openPage,
      closePage,
      setFace,
      cycleFace,
      startAssistant,
      closeAssistant,
      speak,
      runCommand,
      startTimer,
      cancelTimer,
      setAlarm,
      setBattery,
      toggleMusic,
      nextMusic,
      previousMusic,
      stopMusic,
      startPairing,
      notify,
    };
  }

  init();
})();
