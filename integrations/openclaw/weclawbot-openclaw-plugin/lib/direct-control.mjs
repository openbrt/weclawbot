const DEFAULT_CONTEXT = {
  schema: "weclawbot.device_context.v1",
  canvas: { width: 400, height: 300, color: "mono1", refresh: "reflective_slow" },
  content_viewport: {
    id: "content", x: 16, y: 42, width: 368, height: 206,
    format: "mono1", max_pages: 3, auto_page_seconds: 12,
  },
  chrome: { owner: "firmware", reserved: "status_bar,footer" },
  agent_transport: {
    mode: "mqtt_tls_pubsub",
    state: "provisioning_required",
    available: false,
    screen_document_available: false,
    activity_available: false,
    queue_or_mailbox: false,
    delivery: "live_qos1_no_offline_queue",
    session_expiry_seconds: 0,
    recommended_min_update_interval_ms: 60000,
  },
};

export function resolveDeviceContext(value) {
  if (!value || typeof value !== "object" || value.schema !== "weclawbot.device_context.v1") {
    return structuredClone(DEFAULT_CONTEXT);
  }
  return value;
}

export function validateScreenDocument(value, suppliedContext) {
  const context = resolveDeviceContext(suppliedContext);
  const errors = [];
  const pageStats = [];
  const document = value && typeof value === "object" ? value : null;
  const viewport = context.content_viewport;
  if (!document) return result(context, errors.concat("document must be an object"));
  if (document.schema !== "weclawbot.screen_document.v1") {
    errors.push("schema must be weclawbot.screen_document.v1");
  }
  if (document.target !== viewport.id) {
    errors.push(`target must be ${viewport.id}`);
  }
  if (document.kind !== "replace") errors.push("kind must be replace");
  if (!string(document.id)) errors.push("id is required");
  if (typeof document.base_revision !== "string") errors.push("base_revision must be a string (empty for the first document)");
  if ("force_replace" in document && typeof document.force_replace !== "boolean") {
    errors.push("force_replace must be a boolean when present");
  }
  if (!isFutureIso(document.expires_at)) errors.push("expires_at must be a future UTC RFC3339 timestamp");
  if (!Array.isArray(document.pages) || document.pages.length < 1 || document.pages.length > viewport.max_pages) {
    errors.push(`pages must contain 1..${viewport.max_pages} items`);
  } else {
    document.pages.forEach((page, index) => validatePage(page, index, viewport, errors, pageStats));
    if (pageStats.length > 0 && pageStats.every((stat) => stat.uniform)) {
      errors.push("uniform_screen_document: content pages cannot all be a single color; use screen_clear for clearing");
    }
  }
  return result(context, errors, document);
}

function validatePage(page, index, viewport, errors, pageStats) {
  if (!page || typeof page !== "object") {
    errors.push(`pages[${index}] must be an object`);
    return;
  }
  const width = Number(page.width);
  const height = Number(page.height);
  const stride = Number(page.stride);
  if (page.format !== "mono1") errors.push(`pages[${index}].format must be mono1`);
  if (!Number.isInteger(width) || width < 1 || width > viewport.width) {
    errors.push(`pages[${index}].width exceeds viewport`);
  }
  if (!Number.isInteger(height) || height < 1 || height > viewport.height) {
    errors.push(`pages[${index}].height exceeds viewport`);
  }
  const minStride = Number.isInteger(width) ? Math.ceil(width / 8) : 0;
  if (!Number.isInteger(stride) || stride < minStride) {
    errors.push(`pages[${index}].stride must be at least ceil(width / 8)`);
  }
  const bytes = decodeStrictBase64(page.data_b64);
  if (!bytes) {
    errors.push(`pages[${index}].data_b64 is not valid base64`);
  } else if (Number.isInteger(stride) && Number.isInteger(height) && bytes.length !== stride * height) {
    errors.push(`pages[${index}].data_b64 byte length does not match stride * height`);
  } else if (bytes.length > 0) {
    pageStats.push({ index, uniform: bytes.every((byte) => byte === bytes[0]) });
  }
}

function result(context, errors, document = null) {
  const pageCount = Array.isArray(document?.pages) ? document.pages.length : 0;
  const warnings = [];
  if (pageCount === 1) {
    warnings.push("single_page_document: firmware will not split pixels; verify the page remains readable before publishing");
  } else if (pageCount > 1) {
    warnings.push("multi_page_document: firmware will auto-flip pages and manual left/right buttons can change pages");
  }
  return {
    ok: errors.length === 0,
    errors,
    warnings,
    viewport: context.content_viewport,
    layout_guidance: {
      hardware_contract: "agent supplies pre-rendered mono1 pixels; firmware validates geometry and does not lay out text or split pages",
      page_contract: "pages.length is the physical page count; content documents support 1-3 pages",
      preference_policy: "preserve user-agent learned layout preferences unless they violate hardware bounds or the user asks to change them",
      review_policy: "agent should inspect rendered bitmap pages against user preferences and learned standards before publishing when possible",
      max_pages: context.content_viewport?.max_pages,
      content_viewport_px: {
        width: context.content_viewport?.width,
        height: context.content_viewport?.height,
      },
    },
    agent_transport: context.agent_transport || null,
    direct_delivery_ready: context.agent_transport?.screen_document_available === true
      || context.agent_transport?.available === true,
  };
}

function decodeStrictBase64(value) {
  if (!string(value) || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/u.test(value)) {
    return null;
  }
  try {
    return Buffer.from(value, "base64");
  } catch {
    return null;
  }
}

function isFutureIso(value) {
  if (!/^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(?:\.[0-9]{3})?Z$/u.test(String(value || ""))) return false;
  const stamp = Date.parse(value);
  return Number.isFinite(stamp) && stamp > Date.now();
}

function string(value) {
  return typeof value === "string" && value.length > 0;
}
