import { Annotation, END, START, StateGraph } from "@langchain/langgraph";

import { stickyCoreSkill } from "../skills/sticky-core.js";
import { eventToBundle } from "./adapters.js";
import { maybeRouteToModel } from "./model-router.js";
import { validateDecision } from "./validate.js";

const DEFAULT_SKILLS = [stickyCoreSkill];

const AgentState = Annotation.Root({
  event: Annotation({ reducer: (_left, right) => right, default: () => null }),
  options: Annotation({ reducer: (_left, right) => right || {}, default: () => ({}) }),
  skills: Annotation({ reducer: (_left, right) => right || DEFAULT_SKILLS, default: () => DEFAULT_SKILLS }),
  bundle: Annotation({ reducer: (_left, right) => right, default: () => null }),
  skill: Annotation({ reducer: (_left, right) => right, default: () => null }),
  ruleDecision: Annotation({ reducer: (_left, right) => right, default: () => null }),
  decision: Annotation({ reducer: (_left, right) => right, default: () => null }),
  finalDecision: Annotation({ reducer: (_left, right) => right, default: () => null }),
  trace: Annotation({
    reducer: (left, right) => [...(left || []), ...(Array.isArray(right) ? right : [])],
    default: () => [],
  }),
});

const graph = new StateGraph(AgentState)
  .addNode("normalize_event", normalizeEventNode)
  .addNode("select_skill", selectSkillNode)
  .addNode("run_rules", runRulesNode)
  .addNode("route_model", routeModelNode)
  .addNode("validate_decision", validateDecisionNode)
  .addEdge(START, "normalize_event")
  .addEdge("normalize_event", "select_skill")
  .addEdge("select_skill", "run_rules")
  .addEdge("run_rules", "route_model")
  .addEdge("route_model", "validate_decision")
  .addEdge("validate_decision", END)
  .compile();

export async function curateEventWithLangGraph(event, options = {}) {
  const state = await graph.invoke({
    event,
    options,
    skills: options.skills || DEFAULT_SKILLS,
  });
  if (!state.finalDecision) {
    throw new Error("official agent graph completed without a decision");
  }
  return state.finalDecision;
}

export function officialAgentGraph() {
  return graph;
}

async function normalizeEventNode(state) {
  return {
    bundle: eventToBundle(state.event),
    trace: ["graph:normalize_event"],
  };
}

async function selectSkillNode(state) {
  const skill = selectSkill(state.bundle, state.skills);
  return {
    skill,
    trace: [`graph:select_skill:${skill.id}`],
  };
}

async function runRulesNode(state) {
  return {
    ruleDecision: await state.skill.curate(state.bundle, state.options),
    trace: ["graph:run_rules"],
  };
}

async function routeModelNode(state) {
  const options = {
    ...state.options,
    skillId: state.skill.id,
  };
  return {
    decision: await maybeRouteToModel(state.bundle, state.ruleDecision, options),
    trace: ["graph:route_model"],
  };
}

async function validateDecisionNode(state) {
  const includeTrace = Boolean(state.options.includeTrace);
  const graphTrace = [...state.trace, "graph:validate_decision"];
  const decision = {
    ...state.decision,
    version: 1,
    event_id: state.bundle.event_id,
    skill: {
      id: state.skill.id,
      version: state.skill.version,
    },
  };
  if (includeTrace) {
    decision.trace = [
      ...graphTrace,
      ...(Array.isArray(state.decision?.trace) ? state.decision.trace : []),
    ];
  }
  return {
    finalDecision: validateDecision(decision, { includeTrace }),
    trace: ["graph:validate_decision"],
  };
}

function selectSkill(_bundle, skills) {
  if (!Array.isArray(skills) || skills.length === 0) {
    throw new Error("at least one skill is required");
  }
  return skills[0];
}
