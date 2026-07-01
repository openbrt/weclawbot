const CONVERSATION_PROBE_PATTERNS = [
  /^(你好|您好|在吗|测试|测试一下)$/u,
  /^(早|早上好|中午好|晚上好|晚安|拜拜)$/u,
  /^(hello|hi|hey|ping|test)$/iu,
];

const IGNORE_PATTERNS = [
  /^(哈+|哈哈哈*|收到|好的|好|嗯|哦|ok|OK|谢谢|谢了|辛苦了)$/u,
];

const SERVICE_TEXT_PATTERNS = [
  /文件发你了/u,
  /发了(个|一个)?(文件|文档|图片|表格|PPT|PDF)/iu,
  /^(图片|照片|文件|文档|表格|PDF|PPT)$/iu,
];

const VAGUE_PATTERNS = [
  /有个事|有点事|有事|那个事|这件事|这事|一些事|某个事/u,
  /好像|可能|大概|应该/u,
];

const CLEAR_PATTERNS = [
  /^(清屏|清掉|清空|删除|删掉|不要显示了|别显示了|取消上屏)$/u,
  /^(把屏幕清掉|把微笺清掉|清除当前(微笺|记事贴)?)$/u,
];

const SCREEN_INTENT_PATTERNS = [
  /上屏|放到?屏(幕|上)?|发到?屏(幕|上)?|贴到?屏(幕|上)?|显示到?屏(幕|上)?|显示在屏(幕|上)?/u,
  /记到?微笺|记在屏(幕|上)?|写到?屏(幕|上)?/u,
];

const FUTURE_VALUE_PATTERNS = [
  /明天|后天|周[一二三四五六日天]|星期[一二三四五六日天]|[0-9一二三四五六七八九十]{1,2}[点:：]|[0-9]{1,2}月[0-9]{1,2}[日号]?/u,
  /取件|快递|驿站|门禁卡|验证码|取货码|密码|单号|票号/u,
  /会议|开会|待办|提醒|记得|截止|deadline|DDL/iu,
  /停水|停电|物业|通知|缴费|账单/u,
  /买|采购|清单|带上|准备|提交|发送|领取|办理/u,
  /[0-9]{2,}|[A-Z0-9]{4,}/u,
];

export const stickyCoreSkill = {
  id: "sticky-core",
  version: "0.1.8",
  async curate(bundle) {
    const trace = [];
    const text = collectText(bundle);
    const compact = compactText(text);
    const sourceKind = bundle.source.kind || "unknown";

    if (!compact) {
      trace.push("empty_content");
      return serviceRequired(bundle, "empty_content", trace);
    }

    if (isUnsupportedRawAttachment(bundle)) {
      trace.push("raw_attachment_without_extractor");
      return serviceRequired(bundle, "extractor_required", trace);
    }

    if (CLEAR_PATTERNS.some((pattern) => pattern.test(compact))) {
      trace.push("clear_current_note");
      return {
        action: "clear_note",
        user_reply: "已清除屏幕上的微笺。",
        confidence: 0.94,
        trace,
      };
    }

    const conversationReply = replyForConversationProbe(compact);
    if (conversationReply) {
      trace.push("reply_conversation_probe");
      return {
        action: "reply_only",
        user_reply: conversationReply,
        confidence: 0.98,
        trace,
      };
    }

    if (isIgnorable(compact)) {
      trace.push("ignore_low_value_chat");
      return {
        action: "ignore",
        confidence: 0.96,
        trace,
      };
    }

    if (SERVICE_TEXT_PATTERNS.some((pattern) => pattern.test(compact))) {
      trace.push("text_mentions_unsupported_file");
      return serviceRequired(bundle, "file_runtime_required", trace);
    }

    const explicitScreenText = explicitScreenTextFrom(compact);
    if (explicitScreenText) {
      trace.push("explicit_screen_intent");
      const note = createNote(bundle, explicitScreenText);
      return {
        action: currentScreenText(bundle) ? "replace_note" : "create_note",
        note,
        user_reply: createNoteReply(note),
        confidence: 0.9,
        trace,
      };
    }

    if (looksLikeConversation(compact) && !hasScreenIntent(compact)) {
      trace.push("conversation_not_screen_task");
      return replyOnly("我收到了。这条我先按普通对话处理；要我记住、提醒、整理或显示到屏幕时，直接把任务说清楚就行。", 0.62, trace);
    }

    const score = futureValueScore(compact, sourceKind);
    trace.push(`future_value_score:${score}`);

    if (score <= 0) {
      trace.push("not_screen_task");
      return replyOnly("收到。我先不更新屏幕；如果要记录、提醒或上屏，请直接说清楚要处理的内容。", 0.7, trace);
    }

    if (score <= 1 && isVague(compact)) {
      trace.push("ambiguous_text");
      return {
        action: "clarify",
        user_reply: "这条还不够具体。请补充时间、地点或要做的事，我再整理。",
        confidence: 0.72,
        trace,
      };
    }

    const currentText = currentScreenText(bundle);
    const dependsOnCurrent = dependsOnCurrentScreen(compact, bundle);
    if (dependsOnCurrent) {
      const listUpdate = buildCurrentListUpdate(compact, currentText);
      if (listUpdate) {
        trace.push(`current_screen_named_list_add:${listUpdate.listName}`);
        return {
          action: "update_note",
          note: listUpdate.note,
          user_reply: `已把“${listUpdate.items.join("、")}”加入${listUpdate.listName}，并保留其他事项。`,
          confidence: 0.9,
          trace,
        };
      }
    }
    const note = createNote(bundle, compact);
    if (dependsOnCurrent) {
      trace.push("current_screen_update_needs_semantic_curation");
      return {
        action: "clarify",
        user_reply: "我会结合当前屏幕整理这条补充；若暂时无法可靠合并，我会保持原屏幕不变。",
        confidence: 0.45,
        trace,
      };
    }
    if (needsSemanticCuration(compact, sourceKind, bundle)) {
      trace.push("semantic_curation_needed");
      return {
        action: currentText ? "replace_note" : "create_note",
        note,
        user_reply: createNoteReply(note),
        confidence: 0.55,
        trace,
      };
    }

    return {
      action: currentText ? "replace_note" : "create_note",
      note,
      user_reply: createNoteReply(note),
      confidence: Math.min(0.95, 0.68 + score * 0.07),
      trace,
    };
  },
};

function collectText(bundle) {
  return bundle.blocks
    .map((block) => {
      if (block.type === "table" && Array.isArray(block.rows)) {
        return block.rows.map((row) => row.join(" ")).join("\n");
      }
      return block.text || "";
    })
    .filter(Boolean)
    .join("\n");
}

function compactText(text) {
  return normalizeChineseTime(text)
    .replace(/\r/g, "\n")
    .split("\n")
    .map((line) => line.trim())
    .filter(Boolean)
    .join("\n")
    .trim();
}

function isIgnorable(text) {
  const oneLine = text.replace(/\s+/g, "");
  if (IGNORE_PATTERNS.some((pattern) => pattern.test(oneLine))) {
    return true;
  }
  if (/^[\p{Emoji_Presentation}\p{Emoji}\s~!！。,.，…]+$/u.test(text)) {
    return true;
  }
  return oneLine.length <= 2 && !/[0-9一二三四五六七八九十]/u.test(oneLine);
}

function replyOnly(userReply, confidence, trace) {
  return {
    action: "reply_only",
    user_reply: userReply,
    confidence,
    trace,
  };
}

function replyForConversationProbe(text) {
  const oneLine = normalizeConversationProbe(text);
  if (!CONVERSATION_PROBE_PATTERNS.some((pattern) => pattern.test(oneLine))) {
    return "";
  }
  if (/^(测试|测试一下|ping|test)$/iu.test(oneLine)) {
    return "在，微信通道和 WeClawBot 云端都在线。你可以直接说要我记住、整理或提醒的事。";
  }
  if (/^(晚安|拜拜)$/u.test(oneLine)) {
    return oneLine === "晚安" ? "晚安，我在这里。" : "收到，回头见。";
  }
  return "在，我在。你可以直接说要我记住、整理或提醒的事。";
}

function hasScreenIntent(text) {
  return SCREEN_INTENT_PATTERNS.some((pattern) => pattern.test(text));
}

function explicitScreenTextFrom(text) {
  if (!hasScreenIntent(text)) {
    return "";
  }
  const raw = String(text || "").trim();
  const afterCommand = raw.match(/(?:上屏|放到?屏(?:幕|上)?|发到?屏(?:幕|上)?|贴到?屏(?:幕|上)?|显示到?屏(?:幕|上)?|显示在屏(?:幕|上)?|记到?微笺|记在屏(?:幕|上)?|写到?屏(?:幕|上)?)[：:，, ]+(.{1,420})$/u);
  if (afterCommand?.[1]) {
    return afterCommand[1].trim();
  }
  const beforeCommand = raw.match(/^(?:请|帮我|把|将)?(.{1,420}?)(?:上屏|放到?屏(?:幕|上)?|发到?屏(?:幕|上)?|贴到?屏(?:幕|上)?|显示到?屏(?:幕|上)?|显示在屏(?:幕|上)?|记到?微笺|记在屏(?:幕|上)?|写到?屏(?:幕|上)?)(?:吧|一下)?$/u);
  return beforeCommand?.[1]?.trim() || "";
}

function looksLikeConversation(text) {
  const raw = String(text || "").trim();
  if (/[?？]$/u.test(raw)) {
    return true;
  }
  if (/^(?:你|我|我们|这个|那个|为什么|怎么|如何|什么|谁|哪里|在哪|多少|能不能|可以吗|是不是)/u.test(raw)) {
    return true;
  }
  if (/^(?:what|why|how|who|where|when|can|could|would|should|do|does|did|are|is|am)\b/iu.test(raw)) {
    return true;
  }
  return false;
}

function currentScreenText(bundle) {
  return bundle?.screen?.current_note?.canonical_text ||
    bundle?.screen?.current_note?.text || "";
}

function normalizeConversationProbe(text) {
  return String(text || "")
    .toLowerCase()
    .replace(/[\s~!！。,.，、;；:："'“”‘’()[\]{}<>《》?？/\\|-]+/gu, "")
    .trim();
}

function isVague(text) {
  return VAGUE_PATTERNS.some((pattern) => pattern.test(text));
}

function needsSemanticCuration(text, sourceKind, bundle) {
  if (dependsOnCurrentScreen(text, bundle)) {
    return true;
  }
  if (checklistItemCount(text) >= 5 && text.length >= 40) {
    return true;
  }
  if (sourceKind !== "wechat_text" && sourceKind !== "wechat_voice_transcript" &&
      sourceKind !== "text" && sourceKind !== "voice_transcript" && text.length >= 48) {
    return true;
  }
  if (text.length >= 90) {
    return true;
  }
  const sentenceCount = text
    .split(/[。！？!?；;\n]+/u)
    .map((line) => line.trim())
    .filter(Boolean)
    .length;
  return sentenceCount >= 3 && text.length >= 56;
}

function dependsOnCurrentScreen(text, bundle) {
  const current = bundle?.screen?.current_note?.canonical_text ||
    bundle?.screen?.current_note?.text || "";
  if (!current) {
    return false;
  }
  return /(另外|还有|补充|加上|顺便|改成|修改|换成|不要|删掉|去掉|上面|当前|这个|那条|刚才)/u.test(text);
}

function buildCurrentListUpdate(text, currentText) {
  const edit = parseNamedListAddition(text);
  if (!edit || !currentText) {
    return null;
  }
  const targetItems = [];
  const otherItems = [];
  let active = edit.listName;

  for (const line of sourceLines(currentText)) {
    const header = normalizeSectionHeader(line);
    if (header) {
      active = header;
      continue;
    }
    for (const item of splitExistingListLine(line, active, edit.listName)) {
      if (!item || normalizeListName(item) === normalizeListName(edit.listName)) {
        continue;
      }
      const belongsToTarget = normalizeListName(active) === normalizeListName(edit.listName) &&
        !looksLikeNonShoppingTask(item);
      if (belongsToTarget || (!active && !looksLikeNonShoppingTask(item))) {
        pushUnique(targetItems, item);
      } else {
        pushUnique(otherItems, item);
      }
    }
  }

  for (const item of edit.items) {
    pushUnique(targetItems, item);
  }
  if (!targetItems.length) {
    return null;
  }

  const bodyLines = [
    edit.listName,
    ...targetItems.map((item) => `□ ${item}`),
  ];
  if (otherItems.length) {
    bodyLines.push("待办", ...otherItems.map((item) => `□ ${item}`));
  }
  return {
    listName: edit.listName,
    items: edit.items,
    note: {
      template: "sticky.v1",
      title: "微笺",
      body: bodyLines.join("\n"),
      footer: "",
      priority: "normal",
    },
  };
}

function parseNamedListAddition(rawText) {
  const text = String(rawText || "")
    .replace(/\s+/gu, "")
    .replace(/[。.!！?？]+$/gu, "");
  const patterns = [
    /^(?<list>[\p{Script=Han}A-Za-z0-9]{1,12}清单)(?:里|中|上)?(?:再|也|还|顺便)?(?:加上|添加|添上|补上|加入|加一条|加一个|加)(?<items>.+)$/u,
    /^(?:给|帮我给|帮我在|在)(?<list>[\p{Script=Han}A-Za-z0-9]{1,12}清单)(?:里|中|上)?(?:再|也|还|顺便)?(?:加上|添加|添上|补上|加入|加一条|加一个|加)(?<items>.+)$/u,
  ];
  for (const pattern of patterns) {
    const match = text.match(pattern);
    const listName = match?.groups?.list?.trim();
    const items = splitListItems(match?.groups?.items || "")
      .filter((item) => !looksLikeNonShoppingTask(item));
    if (listName && items.length) {
      return { listName, items };
    }
  }
  return null;
}

function structureChecklistBody(text, title) {
  const source = compactText(text);
  if (!source || /[□☐]/u.test(source)) {
    return "";
  }

  const titleHeader = normalizeSectionHeader(title);
  const lines = sourceLines(source);
  const hasExplicitChecklist = lines.some((line) =>
    normalizeSectionHeader(line) || parseInlineChecklistHeader(line),
  ) || /(?:购物清单|采购清单|买菜清单|买东西清单|待办|事项|提醒)/u.test(source);
  if (!hasExplicitChecklist) {
    return "";
  }

  const sections = [];
  let active = titleHeader || "";
  let sawEvidence = Boolean(titleHeader);

  for (const line of lines) {
    const header = normalizeSectionHeader(line);
    if (header) {
      active = normalizeChecklistDisplayHeader(header);
      sawEvidence = true;
      ensureChecklistSection(sections, active);
      continue;
    }

    const inline = parseInlineChecklistHeader(line);
    if (inline) {
      active = normalizeChecklistDisplayHeader(inline.header);
      sawEvidence = true;
      for (const item of splitPlainChecklistItems(inline.rest)) {
        addChecklistItem(sections, active, item);
      }
      continue;
    }

    if (sawEvidence) {
      active = active || "购物清单";
      for (const item of splitPlainChecklistItems(line)) {
        addChecklistItem(sections, active, item);
      }
    }
  }

  if (!sawEvidence) {
    return "";
  }

  const out = [];
  for (const section of sections.filter((item) => item.items.length)) {
    out.push(section.header, ...section.items.map((item) => `□ ${item}`));
  }
  return out.length ? out.join("\n") : "";
}

function parseInlineChecklistHeader(text) {
  const match = String(text || "").trim().match(/^(?<header>[\p{Script=Han}A-Za-z0-9]{0,12}(?:购物清单|采购清单|买菜清单|买东西清单|清单|待办|事项|提醒))\s*[：:，,、 ]+\s*(?<rest>.+)$/u);
  const header = normalizeSectionHeader(match?.groups?.header || "");
  const rest = match?.groups?.rest?.trim() || "";
  return header && rest ? { header, rest } : null;
}

function ensureChecklistSection(sections, header) {
  const displayHeader = normalizeChecklistDisplayHeader(header);
  let section = sections.find((item) => normalizeListName(item.header) === normalizeListName(displayHeader));
  if (!section) {
    section = { header: displayHeader, items: [] };
    sections.push(section);
  }
  return section;
}

function addChecklistItem(sections, activeHeader, item) {
  const normalized = normalizeListItem(item);
  if (!normalized || normalizeSectionHeader(normalized)) {
    return;
  }
  const target = normalizeChecklistDisplayHeader(activeHeader) !== "待办" && looksLikeNonShoppingTask(normalized)
    ? "待办"
    : normalizeChecklistDisplayHeader(activeHeader);
  pushUnique(ensureChecklistSection(sections, target).items, normalized);
}

function normalizeChecklistDisplayHeader(header) {
  const value = normalizeSectionHeader(header) || "清单";
  if (/^(?:清单|购物清单|采购清单|买菜清单|买东西清单)$/u.test(value)) {
    return /(?:采购|买菜|买东西)/u.test(value) ? value : "购物清单";
  }
  if (/^(?:待办|事项|提醒)$/u.test(value)) {
    return "待办";
  }
  return value;
}

function splitPlainChecklistItems(value) {
  const cleaned = String(value || "")
    .replace(/^(?:请|帮我|记得|别忘了|别忘|需要|要)\s*/u, "")
    .replace(/^(?:买|采购|添|添加|加上|补上)\s*/u, "")
    .replace(/[。.!！?？]+$/u, "")
    .trim();
  if (!cleaned) {
    return [];
  }
  const parts = /[、,，；;\n]/u.test(cleaned)
    ? cleaned.split(/[、,，；;\n]+/u)
    : (/\s/u.test(cleaned) ? cleaned.split(/\s+/u) : [cleaned]);
  return parts
    .map(normalizeListItem)
    .filter(Boolean)
    .slice(0, 18);
}

function sourceLines(text) {
  return String(text || "")
    .replace(/\r/g, "\n")
    .split("\n")
    .map((line) => line.replace(/\t/g, " ").trim())
    .filter(Boolean);
}

function normalizeSectionHeader(line) {
  const value = normalizeListItem(line);
  if (/^(?:清单|待办|事项|提醒)$/u.test(value) ||
      /^[\p{Script=Han}A-Za-z0-9]{1,12}(?:清单|待办|事项|提醒)$/u.test(value)) {
    return value;
  }
  return "";
}

function normalizeListName(value) {
  return String(value || "").replace(/[：:□\-—*•\s]/gu, "").trim();
}

function splitExistingListLine(line, active, listName) {
  const withoutBullet = normalizeListItem(line);
  if (!withoutBullet) {
    return [];
  }
  if (normalizeSectionHeader(withoutBullet)) {
    return [];
  }
  if (/[、,，；;]/u.test(withoutBullet)) {
    return withoutBullet.split(/[、,，；;]+/u).map(normalizeListItem).filter(Boolean);
  }
  if (/\s/u.test(withoutBullet) &&
      (normalizeListName(active) === normalizeListName(listName) || !looksLikeNonShoppingTask(withoutBullet))) {
    return withoutBullet.split(/\s+/u).map(normalizeListItem).filter(Boolean);
  }
  return [withoutBullet];
}

function splitListItems(value) {
  return String(value || "")
    .replace(/^(?:一个|一条|一下|：|:|，|,|、)+/u, "")
    .split(/(?:以及|还有|和|、|，|,|；|;)+/u)
    .map(normalizeListItem)
    .filter(Boolean)
    .slice(0, 8);
}

function looksLikeNonShoppingTask(item) {
  const text = String(item || "");
  if (!text) {
    return false;
  }
  if (/^(?:买|采购|订购|预订|订).{1,18}(?:蛋糕|鲜花|水果|牛奶|鸡蛋|面包|药|票|菜|油|米|纸|礼物)$/u.test(text)) {
    return false;
  }
  return /(?:拿|取|还给|还|送|寄|联系|打电话|电话|约|预约|提醒|缴|付款|复诊|上课|开会|提交|回复|门口|保安亭|快递|雨伞|钥匙|证件|资料|报告)/u.test(text);
}

function normalizeListItem(value) {
  return String(value || "")
    .replace(/^[\s\-—*•·●○□☐]+/u, "")
    .replace(/^(?:还有|还要|另外|另加|再加|顺便|记得|请)\s*/u, "")
    .replace(/[。.!！?？]+$/u, "")
    .trim();
}

function pushUnique(list, item) {
  const normalized = normalizeListItem(item);
  if (!normalized) {
    return;
  }
  const key = normalizeListName(normalized);
  if (!list.some((existing) => normalizeListName(existing) === key)) {
    list.push(normalized);
  }
}

function isUnsupportedRawAttachment(bundle) {
  const rawKinds = new Set([
    "wechat_image",
    "wechat_file",
    "image",
    "pdf",
    "docx",
    "pptx",
    "xlsx",
    "csv",
    "raw_voice",
    "unknown_file",
  ]);
  if (!rawKinds.has(bundle.source.kind)) {
    return false;
  }
  return !bundle.blocks.some((block) => block.type !== "metadata" && block.text);
}

function serviceRequired(bundle, reason, trace) {
  const kind = bundle.source.kind || "内容";
  let userReply = `${kind} 需要外部提取能力整理成记事贴。`;
  if (kind === "wechat_image" || kind === "image") {
    userReply = "图片我收到了。当前还在接入 OCR/视觉整理，请先把图片里要贴到屏幕的文字或摘要发来。";
  } else if (kind === "wechat_file" || reason === "file_runtime_required") {
    userReply = "文件暂不能直接上屏。请改发一段要贴出的摘要，或稍后接入 WeClawBot 文件处理服务。";
  } else if (reason === "extractor_required") {
    userReply = "这个文件需要先提取内容，当前不会直接贴到屏幕。请改发可直接显示的摘要。";
  } else if (kind === "wechat_text") {
    userReply = "这条内容需要进一步整理能力，当前不会直接贴到屏幕。";
  }
  return {
    action: "service_required",
    reason,
    user_reply: userReply,
    confidence: 0.9,
    trace,
  };
}

function createNoteReply(note) {
  return `已覆盖到屏幕：${note.body}。不合适可直接发“修改为…”或“清屏”。`;
}

function futureValueScore(text, sourceKind) {
  let score = 0;
  for (const pattern of FUTURE_VALUE_PATTERNS) {
    if (pattern.test(text)) {
      score += 1;
    }
  }
  if (score > 0 && (text.includes("\n") || /[、,，;]/u.test(text))) {
    score += 1;
  }
  if (sourceKind !== "wechat_text" && sourceKind !== "wechat_voice_transcript") {
    score += 1;
  }
  if (score > 0 && text.length >= 24) {
    score += 1;
  }
  return score;
}

function createNote(bundle, text) {
  const title = inferTitle(bundle, text);
  const checklistBody = structureChecklistBody(text, title);
  const body = checklistBody || fitBody(text);
  return {
    template: "sticky.v1",
    title: checklistBody ? "微笺" : title,
    body,
    footer: "",
    priority: inferPriority(text),
  };
}

function inferTitle(bundle, text) {
  const filenameTitle = cleanFilenameTitle(bundle.source.filename);
  if (looksLikeChecklist(text)) {
    return "清单";
  }
  if (/取件|快递|驿站|取货/u.test(text)) {
    return "取件";
  }
  if (/停水|停电|物业|通知/u.test(text)) {
    return filenameTitle || "物业通知";
  }
  if (/会议|开会|待办/u.test(text)) {
    return "会议待办";
  }
  if (/买|采购|清单/u.test(text)) {
    return "清单";
  }
  if (/验证码|门禁卡|密码|码/u.test(text)) {
    return "重要信息";
  }
  return filenameTitle || "记事";
}

function looksLikeChecklist(text) {
  return checklistItemCount(text) >= 4 ||
    (/(清单|检查|待办|出门前|准备)/u.test(text) && /[、,，；;\n]/u.test(text));
}

function checklistItemCount(text) {
  const enumerated = text.match(/(?:^|[、,，；;\s])(?:[0-9]{1,2}|[一二三四五六七八九十])(?:[.．、)]|\s+)/gu);
  if (enumerated && enumerated.length >= 3) {
    return enumerated.length;
  }
  if (!/(清单|检查|待办|出门前|准备|带|买|采购|别忘|记得)/u.test(text)) {
    return 0;
  }
  return text
    .split(/[、,，；;\n]+/u)
    .map((part) => part.trim())
    .filter(Boolean)
    .length;
}

function cleanFilenameTitle(filename) {
  if (!filename) {
    return "";
  }
  return filename.replace(/\.[^.]+$/u, "").trim().slice(0, 18);
}

function inferPriority(text) {
  if (/紧急|马上|立刻|今天|截止|deadline|DDL/iu.test(text)) {
    return "high";
  }
  return "normal";
}

function fitBody(text) {
  const lines = text.split("\n").map((line) => line.trim()).filter(Boolean);
  const normalized = lines.length > 1 ? lines.join("\n") : (lines[0] || text);
  return normalized
    .split("\n")
    .map((line) => line.replace(/[ \t]+/g, " ").trim())
    .filter(Boolean)
    .join("\n")
    .slice(0, 520);
}

function normalizeChineseTime(text) {
  const numerals = {
    一: 1,
    二: 2,
    两: 2,
    三: 3,
    四: 4,
    五: 5,
    六: 6,
    七: 7,
    八: 8,
    九: 9,
    十: 10,
  };
  return text.replace(/([一二两三四五六七八九十])点/gu, (_, n) => ` ${numerals[n]} 点`);
}
