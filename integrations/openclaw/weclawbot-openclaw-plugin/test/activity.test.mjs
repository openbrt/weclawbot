import assert from "node:assert/strict";

import { validateActivity } from "../lib/activity.mjs";

const context = {
  agent_transport: {
    available: true,
    mode: "mqtt_tls_pubsub",
    queue_or_mailbox: false,
  },
};
const thinking = validateActivity({
  schema: "weclawbot.activity.v1",
  state: "thinking",
  correlation_id: "task-1",
  ttl_seconds: 45,
}, context);
assert.equal(thinking.ok, true, thinking.errors.join("; "));
assert.equal(thinking.direct_delivery_ready, true);

const idle = validateActivity({
  schema: "weclawbot.activity.v1",
  state: "idle",
  correlation_id: "task-1",
}, context);
assert.equal(idle.ok, true, idle.errors.join("; "));

const invalid = validateActivity({
  schema: "weclawbot.activity.v1",
  state: "thinking",
  correlation_id: "task-1",
  ttl_seconds: 500,
}, context);
assert.equal(invalid.ok, false);
assert.ok(invalid.errors.some((error) => error.includes("ttl_seconds")));

console.log("activity validator: ok");
