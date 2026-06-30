import { eventToBundle } from "./adapters.js";
import { curateEventWithLangGraph } from "./langgraph-agent.js";
import { maybeRouteToModel } from "./model-router.js";
import { validateDecision } from "./validate.js";
import { stickyCoreSkill } from "../skills/sticky-core.js";

const DEFAULT_SKILLS = [stickyCoreSkill];

export async function curateEvent(event, options = {}) {
  if (options.useLangGraph !== false && options.agentFramework !== "legacy") {
    return curateEventWithLangGraph(event, options);
  }
  return curateEventLegacy(event, options);
}

export async function curateEventLegacy(event, options = {}) {
  const bundle = eventToBundle(event);
  const skill = selectSkill(bundle, options.skills || DEFAULT_SKILLS);
  const ruleDecision = await skill.curate(bundle, options);
  const decision = await maybeRouteToModel(bundle, ruleDecision, options);
  return validateDecision({
    ...decision,
    version: 1,
    event_id: bundle.event_id,
    skill: {
      id: skill.id,
      version: skill.version,
    },
  }, { includeTrace: Boolean(options.includeTrace) });
}

function selectSkill(_bundle, skills) {
  if (!Array.isArray(skills) || skills.length === 0) {
    throw new Error("at least one skill is required");
  }
  return skills[0];
}
