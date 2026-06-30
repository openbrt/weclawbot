const ui = {
  serialState: document.querySelector("#serial-state"),
  connect: document.querySelector("#connect-button"),
  disconnect: document.querySelector("#disconnect-button"),
  refresh: document.querySelector("#refresh-button"),
  saveWifi: document.querySelector("#save-wifi-button"),
  reboot: document.querySelector("#reboot-button"),
  resetModeConfig: document.querySelector("#reset-mode-config-button"),
  clearWifi: document.querySelector("#clear-wifi-button"),
  clearNotes: document.querySelector("#clear-notes-button"),
  clearConsole: document.querySelector("#clear-console-button"),
  serialHelp: document.querySelector("#serial-help"),
  wifiForm: document.querySelector("#wifi-form"),
  ssid: document.querySelector("#ssid-input"),
  password: document.querySelector("#password-input"),
  passwordToggle: document.querySelector("#password-toggle"),
  curatorUrl: document.querySelector("#curator-url-input"),
  modeOfficial: document.querySelector("#agent-mode-official"),
  modeByoa: document.querySelector("#agent-mode-byoa"),
  modeHelp: document.querySelector("#agent-mode-help"),
  console: document.querySelector("#console-output"),
  deviceName: document.querySelector("#device-name"),
  wifiState: document.querySelector("#wifi-state"),
  ipAddress: document.querySelector("#ip-address"),
  curatorState: document.querySelector("#curator-state"),
  wechatState: document.querySelector("#wechat-state"),
  noteState: document.querySelector("#note-state"),
  firmwareVersion: document.querySelector("#firmware-version"),
};

let port = null;
let reader = null;
let writer = null;
let inputDone = null;
let lineBuffer = "";
let connecting = false;
let lastErrorMessage = "";
let statusTimer = null;
let quietStatusResponse = false;
const encoder = new TextEncoder();
const maxConsoleLines = 240;
const defaultGatewayUrl = "https://weclawbot.link/gateway";
const byoaGatewayUrl = "https://weclawbot.link/byoa";
let currentAgentMode = "official";
let agentModeDirty = false;
let rebootAfterSave = false;
let knownWifiConfigured = false;
let knownWifiSsid = "";

function agentModeForUrl(url) {
  return url === byoaGatewayUrl ? "byoa" : "official";
}

function setAgentMode(mode, { syncUrl = true, dirty = false } = {}) {
  const custom = mode === "byoa";
  currentAgentMode = custom ? "byoa" : "official";
  if (dirty) {
    agentModeDirty = true;
  }
  ui.modeOfficial.classList.toggle("is-selected", !custom);
  ui.modeOfficial.setAttribute("aria-pressed", String(!custom));
  ui.modeByoa.classList.toggle("is-selected", custom);
  ui.modeByoa.setAttribute("aria-pressed", String(custom));
  ui.modeHelp.textContent = custom
    ? "保存并重启后，屏幕显示六位绑定码；在你的 Agent 中安装 weclawbotctl 并输入该码配对。"
    : "保存并重启后，屏幕显示微信二维码；扫码即可由 WeClawBot 官方智能体处理并上屏。";
  updateModeResetAction(custom);
  if (syncUrl) {
    const url = custom ? byoaGatewayUrl : defaultGatewayUrl;
    ui.curatorUrl.value = url;
  }
}

function updateModeResetAction(custom) {
  const icon = custom ? "link-2-off" : "log-out";
  const label = custom ? "重置智能体配对" : "重置微信登录";
  const title = custom ? "清除旧配对，保存自定义智能体方式并重启" : "清除微信登录，保存官方方式并重启";
  ui.resetModeConfig.title = title;
  ui.resetModeConfig.innerHTML = `<i data-lucide="${icon}"></i><span>${label}</span>`;
  if (window.lucide) {
    window.lucide.createIcons();
  }
}

function setConnected(connected) {
  ui.serialState.textContent = connected ? "已连接" : "未连接";
  ui.connect.disabled = connected;
  ui.disconnect.disabled = !connected;
  ui.refresh.disabled = !connected;
  ui.saveWifi.disabled = !connected;
  ui.reboot.disabled = !connected;
  ui.resetModeConfig.disabled = !connected;
  ui.clearWifi.disabled = !connected;
  ui.clearNotes.disabled = !connected;
}

function appendConsole(kind, text) {
  if (kind === "error" && text === lastErrorMessage) {
    return;
  }
  lastErrorMessage = kind === "error" ? text : "";
  const line = document.createElement("div");
  line.className = kind ? `line-${kind}` : "";
  line.textContent = text;
  ui.console.appendChild(line);
  while (ui.console.children.length > maxConsoleLines) {
    ui.console.firstChild.remove();
  }
  ui.console.scrollTop = ui.console.scrollHeight;
}

function showSerialError(error) {
  if (error?.name === "NotFoundError") {
    const message = "未选择串口。Codex 内置浏览器可能无法弹出设备授权，请用系统 Chrome 打开 http://localhost:8765/。";
    ui.serialHelp.textContent = message;
    appendConsole("error", message);
    return;
  }
  if (error?.name === "NetworkError") {
    appendConsole("error", "串口正被其他程序占用。请关闭串口监视器后重试。");
    return;
  }
  appendConsole("error", error?.message || "串口连接失败");
}

function setStatus(message) {
  appendConsole("rx", message);
}

function displayValue(value) {
  if (value === undefined || value === null || value === "") {
    return "-";
  }
  return String(value);
}

function applyMessage(message) {
  if (!message.ok) {
    appendConsole("error", message.message || message.code || "设备返回错误");
    return;
  }

  if (message.type === "hello") {
    ui.deviceName.textContent = `${message.name || "WeClawBot"} ${message.chip || ""}`.trim();
    ui.firmwareVersion.textContent = displayValue(message.version);
    return;
  }

  if (message.type === "set") {
    setStatus(message.message || "配置已保存");
    agentModeDirty = false;
    if (rebootAfterSave) {
      rebootAfterSave = false;
      window.setTimeout(() => {
        sendCommand("REBOOT").catch(showSerialError);
      }, 250);
    }
    return;
  }

  if (message.type === "config") {
    knownWifiConfigured = Boolean(message.wifi_configured);
    knownWifiSsid = message.wifi_ssid || "";
    ui.deviceName.textContent = displayValue(message.board || message.name);
    ui.firmwareVersion.textContent = displayValue(message.version);
    ui.wifiState.textContent = message.wifi_configured
      ? `${message.wifi_ssid || "已配置"} / ${
          message.wifi_connected ? "已连接" : message.wifi_error || "未连接"
        }`
      : "未配置";
    ui.ipAddress.textContent = displayValue(message.ip);
    const currentGateway = message.curator_url || defaultGatewayUrl;
    const currentMode = message.agent_mode || agentModeForUrl(currentGateway);
    if (!message.curator_enabled) {
      ui.curatorState.textContent = "未配置";
    } else if (currentMode === "byoa") {
      const pairingLeft = message.agent_pairing_seconds_left > 0
        ? `（有效期 ${Math.floor(message.agent_pairing_seconds_left / 60)}:${String(message.agent_pairing_seconds_left % 60).padStart(2, "0")}）`
        : "";
      ui.curatorState.textContent = message.agent_paired
        ? message.agent_mqtt_connected
          ? `自定义智能体 / 已接管${message.agent_last_status_kind ? ` / ${message.agent_last_status_kind}` : ""}`
          : "自定义智能体 / 已配对，正在重连"
        : message.agent_pairing_code
          ? `自定义智能体 / 绑定码 ${message.agent_pairing_code}${pairingLeft}`
          : "自定义智能体 / 正在准备绑定";
    } else {
      ui.curatorState.textContent = message.agent_mqtt_connected
        ? "WeClawBot 官方 / MQTT 在线"
        : message.agent_paired
          ? "WeClawBot 官方 / 正在重连"
          : "WeClawBot 官方 / 正在准备";
    }
    const qrTime = message.wechat_qr_seconds_left > 0
      ? ` (${Math.floor(message.wechat_qr_seconds_left / 60)}:${String(message.wechat_qr_seconds_left % 60).padStart(2, "0")})`
      : "";
    ui.wechatState.textContent = currentMode === "byoa"
      ? "配对码入口 / 微信忽略"
      : `微信扫码入口 / ${message.wechat_state || (message.wechat_connected ? "已连接" : "未连接")}${qrTime}`;
    if (message.note_count) {
      const revision = message.screen_revision ? ` / ${message.screen_revision.slice(0, 8)}` : "";
      ui.noteState.textContent = `有内容${revision}`;
    } else {
      ui.noteState.textContent = "无内容";
    }
    if (message.wifi_ssid && !ui.ssid.value) {
      ui.ssid.value = message.wifi_ssid;
    }
    ui.password.placeholder = message.wifi_configured ? "已保存，留空不修改" : "Password";
    if (!agentModeDirty && document.activeElement !== ui.curatorUrl) {
      ui.curatorUrl.value = currentGateway;
      setAgentMode(currentMode, { syncUrl: false });
    }
    return;
  }

  if (message.message) {
    setStatus(message.message);
  }
}

function processLine(rawLine) {
  const line = rawLine.trim();
  if (!line) {
    return;
  }
  if (!line.startsWith("WEC:")) {
    appendConsole("", line);
    const gotIp = line.match(/(?:WifiManager: Got IP|sta ip:)\s*([0-9.]+)/);
    if (gotIp) {
      ui.ipAddress.textContent = gotIp[1];
      const ssid = ui.ssid.value.trim() || "Wi-Fi";
      ui.wifiState.textContent = `${ssid} / 已连接`;
      ui.serialHelp.textContent = `Wi-Fi 已连接，设备 IP：${gotIp[1]}`;
    }
    return;
  }

  try {
    const message = JSON.parse(line.slice(4));
    if (!(quietStatusResponse && message.type === "config")) {
      appendConsole("rx", line);
    }
    if (message.type === "config") {
      quietStatusResponse = false;
    }
    applyMessage(message);
  } catch (error) {
    appendConsole("error", `JSON 解析失败: ${error.message}`);
  }
}

async function readLoop() {
  const decoder = new TextDecoderStream();
  inputDone = port.readable.pipeTo(decoder.writable);
  reader = decoder.readable.getReader();

  try {
    while (true) {
      const { value, done } = await reader.read();
      if (done) {
        break;
      }
      lineBuffer += value;
      const lines = lineBuffer.split(/\r?\n/);
      lineBuffer = lines.pop() || "";
      for (const line of lines) {
        processLine(line);
      }
    }
  } catch (error) {
    if (port) {
      appendConsole("error", `串口读取中断: ${error.message}`);
    }
  }
}

async function sendCommand(command, displayCommand = command, logCommand = true) {
  if (!writer) {
    appendConsole("error", "请先连接设备");
    return;
  }
  if (logCommand) {
    appendConsole("tx", `WEC:${displayCommand}`);
  }
  await writer.write(encoder.encode(`WEC:${command}\n`));
}

function startStatusPolling() {
  stopStatusPolling();
  statusTimer = window.setInterval(() => {
    if (!writer) {
      return;
    }
    quietStatusResponse = true;
    sendCommand("GET", "GET", false).catch(showSerialError);
  }, 3000);
}

function stopStatusPolling() {
  if (statusTimer !== null) {
    window.clearInterval(statusTimer);
    statusTimer = null;
  }
  quietStatusResponse = false;
}

async function connectSerial() {
  if (connecting || port) {
    return;
  }
  if (!("serial" in navigator)) {
    appendConsole("error", "当前浏览器不支持 Web Serial");
    return;
  }

  connecting = true;
  ui.connect.disabled = true;
  ui.serialState.textContent = "选择串口中";
  try {
    const authorizedPorts = await navigator.serial.getPorts();
    port = authorizedPorts.length === 1 ? authorizedPorts[0] : await navigator.serial.requestPort();
    await port.open({ baudRate: 115200 });
    if (port.setSignals) {
      await port.setSignals({ dataTerminalReady: false, requestToSend: false });
    }
    writer = port.writable.getWriter();
    setConnected(true);
    ui.serialHelp.textContent = "设备已连接，可读取状态或保存 Wi-Fi。";
    appendConsole("rx", authorizedPorts.length === 1 ? "已复用授权串口" : "串口已打开");
    readLoop();

    await new Promise((resolve) => setTimeout(resolve, 250));
    await sendCommand("HELLO");
    await sendCommand("GET");
    startStatusPolling();
  } catch (error) {
    port = null;
    writer = null;
    setConnected(false);
    showSerialError(error);
  } finally {
    connecting = false;
  }
}

async function disconnectSerial() {
  try {
    stopStatusPolling();
    if (reader) {
      await reader.cancel();
      reader.releaseLock();
      reader = null;
    }
    if (inputDone) {
      await inputDone.catch(() => {});
      inputDone = null;
    }
    if (writer) {
      writer.releaseLock();
      writer = null;
    }
    if (port) {
      if (port.setSignals) {
        await port.setSignals({ dataTerminalReady: false, requestToSend: false });
      }
      await port.close();
      port = null;
    }
  } finally {
    setConnected(false);
    appendConsole("rx", "串口已断开");
  }
}

async function saveWifi(event) {
  event.preventDefault();
  const ssid = ui.ssid.value.trim();
  const password = ui.password.value;
  const wifiNeedsSave = Boolean(password) || !knownWifiConfigured || (ssid && ssid !== knownWifiSsid);
  if (wifiNeedsSave && !ssid) {
    appendConsole("error", "请输入 Wi-Fi 名称");
    ui.ssid.focus();
    return;
  }
  if (!wifiNeedsSave && !currentAgentMode) {
    appendConsole("error", "至少填写一项配置");
    ui.ssid.focus();
    return;
  }

  const payload = { agent_mode: currentAgentMode };
  if (wifiNeedsSave) {
    payload.ssid = ssid;
    payload.password = password;
  }
  const command = `SET ${JSON.stringify(payload)}`;
  const masked = `SET ${JSON.stringify({
    ...payload,
    password: payload.password ? "********" : "",
    ai_token: payload.ai_token ? "********" : undefined,
  })}`;
  rebootAfterSave = true;
  await sendCommand(command, masked);
}

async function confirmAndSend(message, command) {
  if (!window.confirm(message)) {
    return;
  }
  await sendCommand(command);
}

async function resetModeConfig() {
  const custom = currentAgentMode === "byoa";
  const message = custom
    ? "重置自定义智能体配对？页面会清除旧配对，保存自定义智能体方式并重启，随后重新显示六位配对码。"
    : "重置微信登录状态？页面会清除微信登录，保存 WeClawBot 官方方式并重启，随后重新显示二维码。";
  if (!window.confirm(message)) {
    return;
  }
  await sendCommand(custom ? "CLEAR_AGENT" : "CLEAR_WECHAT");
  rebootAfterSave = true;
  await sendCommand(`SET ${JSON.stringify({ agent_mode: currentAgentMode })}`);
}

ui.connect.addEventListener("click", () => {
  connectSerial().catch(showSerialError);
});
ui.disconnect.addEventListener("click", () => {
  disconnectSerial().catch((error) => appendConsole("error", error.message));
});
ui.refresh.addEventListener("click", () => {
  sendCommand("GET").catch((error) => appendConsole("error", error.message));
});
ui.wifiForm.addEventListener("submit", (event) => {
  saveWifi(event).catch((error) => appendConsole("error", error.message));
});
ui.reboot.addEventListener("click", () => {
  sendCommand("REBOOT").catch((error) => appendConsole("error", error.message));
});
ui.resetModeConfig.addEventListener("click", () => {
  resetModeConfig().catch((error) => appendConsole("error", error.message));
});
ui.clearWifi.addEventListener("click", () => {
  confirmAndSend("清除设备上的 Wi-Fi 配置？", "CLEAR_WIFI").catch((error) => appendConsole("error", error.message));
});
ui.clearNotes.addEventListener("click", () => {
  confirmAndSend("清除屏幕上的当前微笺？", "CLEAR_NOTES").catch((error) => appendConsole("error", error.message));
});
ui.clearConsole.addEventListener("click", () => {
  ui.console.replaceChildren();
});
function bindSecretToggle(input, button, showTitle, hideTitle) {
  button.addEventListener("click", () => {
    const visible = input.type === "text";
    input.type = visible ? "password" : "text";
    button.title = visible ? showTitle : hideTitle;
    button.setAttribute("aria-label", button.title);
    button.setAttribute("aria-pressed", String(!visible));
    button.innerHTML = `<i data-lucide="${visible ? "eye" : "eye-off"}"></i>`;
    if (window.lucide) {
      window.lucide.createIcons();
    }
    input.focus();
  });
}
bindSecretToggle(ui.password, ui.passwordToggle, "显示密码", "隐藏密码");
ui.modeOfficial.addEventListener("click", () => setAgentMode("official", { dirty: true }));
ui.modeByoa.addEventListener("click", () => setAgentMode("byoa", { dirty: true }));

window.addEventListener("DOMContentLoaded", () => {
  setConnected(false);
  setAgentMode("official");
  if (window.lucide) {
    window.lucide.createIcons();
  }
  if (!("serial" in navigator)) {
    ui.connect.disabled = true;
    appendConsole("error", "请使用支持 Web Serial 的桌面浏览器");
  }
});
