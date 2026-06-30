#!/usr/bin/env node
// Idle-window precipitation job. Promotes behaviorally-grounded teacher verdicts
// and user corrections into the Skill Library few-shot examples. Run off the hot
// path (cron / sleep window), the way MetaClaw defers updates to idle time.
//
//   node src/cli/precipitate.mjs [skillId]
//
// The single health metric to watch over time is the teacher hit-rate: if the
// student keeps escalating to the teacher at the same rate, precipitation is not
// working. It should trend toward zero.

import { precipitate } from "../runtime/precipitation.js";

const skillId = process.argv[2] || "sticky-core";
const result = precipitate({ skillId });

process.stdout.write(`precipitated ${result.promoted} example(s) into ${result.skillId}\n`);
for (const item of result.items) {
  process.stdout.write(`  + via ${item.via}: ${JSON.stringify(item.example.input)} -> ${item.example.decision.action}\n`);
}
