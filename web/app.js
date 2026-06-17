const ui = {
  serialState: document.querySelector("#serial-state"),
  connect: document.querySelector("#connect-button"),
  disconnect: document.querySelector("#disconnect-button"),
  refresh: document.querySelector("#refresh-button"),
  saveWifi: document.querySelector("#save-wifi-button"),
  reboot: document.querySelector("#reboot-button"),
  clearWifi: document.querySelector("#clear-wifi-button"),
  clearWechat: document.querySelector("#clear-wechat-button"),
  clearNotes: document.querySelector("#clear-notes-button"),
  clearConsole: document.querySelector("#clear-console-button"),
  serialHelp: document.querySelector("#serial-help"),
  wifiForm: document.querySelector("#wifi-form"),
  ssid: document.querySelector("#ssid-input"),
  password: document.querySelector("#password-input"),
  passwordToggle: document.querySelector("#password-toggle"),
  curatorUrl: document.querySelector("#curator-url-input"),
  aiProvider: document.querySelector("#ai-provider-select"),
  aiToken: document.querySelector("#ai-token-input"),
  aiTokenToggle: document.querySelector("#ai-token-toggle"),
  aiEndpoint: document.querySelector("#ai-endpoint-input"),
  aiModel: document.querySelector("#ai-model-input"),
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

function setConnected(connected) {
  ui.serialState.textContent = connected ? "已连接" : "未连接";
  ui.connect.disabled = connected;
  ui.disconnect.disabled = !connected;
  ui.refresh.disabled = !connected;
  ui.saveWifi.disabled = !connected;
  ui.reboot.disabled = !connected;
  ui.clearWifi.disabled = !connected;
  ui.clearWechat.disabled = !connected;
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

  if (message.type === "config") {
    ui.deviceName.textContent = displayValue(message.board || message.name);
    ui.firmwareVersion.textContent = displayValue(message.version);
    ui.wifiState.textContent = message.wifi_configured
      ? `${message.wifi_ssid || "已配置"} / ${
          message.wifi_connected ? "已连接" : message.wifi_error || "未连接"
        }`
      : "未配置";
    ui.ipAddress.textContent = displayValue(message.ip);
    ui.curatorState.textContent = message.curator_enabled
      ? `${message.ai_provider || "weclawbot"} / ${
          message.ai_token_configured ? "token 已保存" : "无 token"
        }`
      : "未配置";
    const qrTime = message.wechat_qr_seconds_left > 0
      ? ` (${Math.floor(message.wechat_qr_seconds_left / 60)}:${String(message.wechat_qr_seconds_left % 60).padStart(2, "0")})`
      : "";
    ui.wechatState.textContent = `${message.wechat_state || (message.wechat_connected ? "已连接" : "未连接")}${qrTime}`;
    ui.noteState.textContent = message.note_count ? "有内容" : "无内容";
    if (message.wifi_ssid && !ui.ssid.value) {
      ui.ssid.value = message.wifi_ssid;
    }
    if (document.activeElement !== ui.curatorUrl) {
      ui.curatorUrl.value = message.curator_url || "";
    }
    if (ui.aiProvider && document.activeElement !== ui.aiProvider) {
      ui.aiProvider.value = message.ai_provider || "weclawbot";
    }
    if (ui.aiEndpoint && document.activeElement !== ui.aiEndpoint) {
      ui.aiEndpoint.value = message.ai_endpoint || "";
    }
    if (ui.aiModel && document.activeElement !== ui.aiModel) {
      ui.aiModel.value = message.ai_model || "";
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
  const curator_url = ui.curatorUrl.value.trim();
  const ai_provider = ui.aiProvider.value || "weclawbot";
  const ai_token = ui.aiToken.value;
  const ai_endpoint = ui.aiEndpoint.value.trim();
  const ai_model = ui.aiModel.value.trim();
  if (!ssid && !curator_url && !ai_provider && !ai_endpoint && !ai_model && !ai_token) {
    appendConsole("error", "至少填写一项配置");
    ui.ssid.focus();
    return;
  }

  const payload = { curator_url, ai_provider, ai_endpoint, ai_model };
  if (ssid) {
    payload.ssid = ssid;
    payload.password = password;
  }
  if (ai_token) {
    payload.ai_token = ai_token;
  }
  const command = `SET ${JSON.stringify(payload)}`;
  const masked = `SET ${JSON.stringify({
    ...payload,
    password: payload.password ? "********" : "",
    ai_token: payload.ai_token ? "********" : undefined,
  })}`;
  await sendCommand(command, masked);
  if (ui.aiToken.value) {
    ui.aiToken.value = "";
    ui.aiToken.placeholder = "token 已发送保存；留空表示不修改";
  }
}

async function confirmAndSend(message, command) {
  if (!window.confirm(message)) {
    return;
  }
  await sendCommand(command);
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
ui.clearWifi.addEventListener("click", () => {
  confirmAndSend("清除设备上的 Wi-Fi 配置？", "CLEAR_WIFI").catch((error) => appendConsole("error", error.message));
});
ui.clearWechat.addEventListener("click", () => {
  confirmAndSend("清除设备上的微信登录状态？", "CLEAR_WECHAT").catch((error) => appendConsole("error", error.message));
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
bindSecretToggle(ui.aiToken, ui.aiTokenToggle, "显示 token", "隐藏 token");

ui.aiProvider.addEventListener("change", () => {
  if (ui.aiProvider.value === "weclawbot") {
    ui.aiToken.placeholder = "WeClawBot 托管模式可留空";
  } else {
    ui.aiToken.placeholder = "填自己的 API key / access token";
  }
  if (window.lucide) {
    window.lucide.createIcons();
  }
});

window.addEventListener("DOMContentLoaded", () => {
  setConnected(false);
  if (window.lucide) {
    window.lucide.createIcons();
  }
  if (!("serial" in navigator)) {
    ui.connect.disabled = true;
    appendConsole("error", "请使用支持 Web Serial 的桌面浏览器");
  }
});
