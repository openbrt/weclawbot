#!/usr/bin/env node

import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const code = process.argv[2] || "";
const agentName = process.argv[3] || process.env.WEC_AGENT_NAME || "user-agent";
const ctl = fileURLToPath(new URL("./weclawbotctl.mjs", import.meta.url));
const child = spawn(process.execPath, [ctl, "bind", code, "--name", agentName], { stdio: "inherit" });
child.on("exit", (status) => process.exit(status ?? 1));
