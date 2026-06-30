import mqtt from "mqtt";

export async function publishControl(credentials, control) {
  const config = normalizeCredentials(credentials);
  validateControl(control);
  const client = connectMqtt(config);
  try {
    await onceConnected(client);
    await publishJson(client, config.controlTopic, control);
  } finally {
    client.end(true);
  }
}

export async function publishControlAndWaitStatus(credentials, control, options = {}) {
  const config = normalizeCredentials(credentials);
  validateControl(control);
  if (!config.statusTopic) throw new Error("agent_status_topic_missing");
  const timeoutMs = Math.max(1000, Number(options.timeoutMs || 12_000));
  const expectedDetail = string(options.expectedDetail);
  const client = connectMqtt(config);
  try {
    await onceConnected(client);
    await subscribe(client, config.statusTopic);
    const statusPromise = waitForStatus(client, {
      timeoutMs,
      expectedDetail,
    });
    await publishJson(client, config.controlTopic, control);
    const status = await statusPromise;
    return { ok: status.kind === "applied", status };
  } finally {
    client.end(true);
  }
}

export async function testConnection(credentials) {
  const config = normalizeCredentials(credentials);
  const client = connectMqtt(config);
  try {
    await onceConnected(client);
  } finally {
    client.end(true);
  }
  return {
    ok: true,
    url: config.url,
    client_id: config.clientId,
    username: config.username,
    control_topic: config.controlTopic,
  };
}

export function normalizeCredentials(value) {
  const mqttConfig = value?.mqtt && typeof value.mqtt === "object" ? value.mqtt : value;
  const topics = mqttConfig?.topics;
  const url = string(mqttConfig?.url);
  const username = string(mqttConfig?.username);
  const password = string(mqttConfig?.password);
  const clientId = string(mqttConfig?.client_id);
  const controlTopic = string(topics?.control);
  const statusTopic = string(topics?.status);
  if (!url || !username || !password || !clientId || !controlTopic) {
    throw new Error("agent_credentials_incomplete");
  }
  if (!/^wss:\/\//u.test(url)) throw new Error("agent_mqtt_requires_wss");
  return { url, username, password, clientId, controlTopic, statusTopic };
}

function connectMqtt(config) {
  return mqtt.connect(config.url, {
    clientId: config.clientId,
    username: config.username,
    password: config.password,
    clean: true,
    reconnectPeriod: 0,
    connectTimeout: 12_000,
    protocolVersion: 5,
    properties: { sessionExpiryInterval: 0 },
  });
}

function validateControl(control) {
  if (!control || typeof control !== "object" || control.schema !== "weclawbot.control.v1") {
    throw new Error("invalid_control_message");
  }
}

function publishJson(client, topic, value) {
  return new Promise((resolve, reject) => {
    client.publish(topic, JSON.stringify(value), { qos: 1, retain: false }, (error) => {
      if (error) reject(error);
      else resolve();
    });
  });
}

function subscribe(client, topic) {
  return new Promise((resolve, reject) => {
    client.subscribe(topic, { qos: 1 }, (error) => {
      if (error) reject(error);
      else resolve();
    });
  });
}

function waitForStatus(client, { timeoutMs, expectedDetail }) {
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => finish(new Error("device_status_timeout")), timeoutMs);
    const finish = (error, status) => {
      clearTimeout(timeout);
      client.removeListener("message", onMessage);
      client.removeListener("error", onError);
      if (error) reject(error);
      else resolve(status);
    };
    const onError = (error) => finish(error);
    const onMessage = (_topic, payload) => {
      const status = parseStatus(payload);
      if (!status) return;
      if (status.kind === "applied") {
        if (expectedDetail && status.detail !== expectedDetail) return;
        finish(null, status);
      } else if (status.kind === "rejected") {
        finish(null, status);
      }
    };
    client.on("message", onMessage);
    client.once("error", onError);
  });
}

function parseStatus(payload) {
  try {
    const status = JSON.parse(payload.toString("utf8"));
    if (status?.schema !== "weclawbot.device_status.v1") return null;
    if (status.kind !== "applied" && status.kind !== "rejected") return null;
    return {
      schema: status.schema,
      kind: status.kind,
      device_id: string(status.device_id),
      detail: string(status.detail),
    };
  } catch {
    return null;
  }
}

function onceConnected(client) {
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => finish(new Error("mqtt_connect_timeout")), 12_500);
    const finish = (error) => {
      clearTimeout(timeout);
      client.removeListener("connect", onConnect);
      client.removeListener("error", onError);
      if (error) reject(error);
      else resolve();
    };
    const onConnect = () => finish();
    const onError = (error) => finish(error);
    client.once("connect", onConnect);
    client.once("error", onError);
  });
}

function string(value) {
  return typeof value === "string" ? value.trim() : "";
}
