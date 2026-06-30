const STATES = new Set(["thinking", "idle"]);

export function validateActivity(value, suppliedContext) {
  const context = suppliedContext && typeof suppliedContext === "object" ? suppliedContext : {};
  const transport = context.agent_transport || null;
  const activity = value && typeof value === "object" ? value : null;
  const errors = [];
  if (!activity) {
    errors.push("activity must be an object");
  } else {
    if (activity.schema !== "weclawbot.activity.v1") {
      errors.push("schema must be weclawbot.activity.v1");
    }
    if (!STATES.has(activity.state)) {
      errors.push("state must be thinking or idle");
    }
    if (typeof activity.correlation_id !== "string" || !activity.correlation_id.trim()) {
      errors.push("correlation_id is required");
    }
    if (activity.state === "thinking") {
      const ttl = Number(activity.ttl_seconds);
      if (!Number.isInteger(ttl) || ttl < 5 || ttl > 120) {
        errors.push("thinking ttl_seconds must be an integer from 5 to 120");
      }
    }
  }
  return {
    ok: errors.length === 0,
    errors,
    agent_transport: transport,
    direct_delivery_ready: transport?.activity_available === true || transport?.available === true,
    delivery: { qos: 1, retained: false, clean_start: true, session_expiry_seconds: 0 },
  };
}
