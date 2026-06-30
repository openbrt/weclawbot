import assert from "node:assert/strict";

import { classifyMqttControlError, normalizeMqttControlError } from "../lib/mqtt-control.mjs";

const revoked = classifyMqttControlError(new Error("Connection refused: Not authorized"));
assert.equal(revoked.code, "credential_revoked_or_not_current_owner");
assert.equal(revoked.message, "credential_revoked_or_not_current_owner");
assert.match(revoked.detail, /Not authorized/u);

const normalized = normalizeMqttControlError(new Error("Connection refused: Not authorized"));
assert.equal(normalized.message, "credential_revoked_or_not_current_owner");
assert.equal(normalized.code, "credential_revoked_or_not_current_owner");
assert.match(normalized.detail, /Not authorized/u);

const reclassified = classifyMqttControlError(normalized);
assert.equal(reclassified.code, "credential_revoked_or_not_current_owner");
assert.match(reclassified.detail, /Not authorized/u);

const timeout = classifyMqttControlError(new Error("mqtt_connect_timeout"));
assert.equal(timeout.code, "mqtt_connect_timeout");

const other = classifyMqttControlError(new Error("socket hang up"));
assert.equal(other.code, "mqtt_unavailable");
assert.equal(other.message, "socket hang up");

console.log("mqtt-control errors: ok");
