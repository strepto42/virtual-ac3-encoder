import { execSync, spawn } from "node:child_process";
import { existsSync } from "node:fs";

function getGhCommand() {
  if (process.platform !== "win32") return "gh";

  const winPaths = [
    "C:\\Program Files\\GitHub CLI\\gh.exe",
    "C:\\Program Files (x86)\\GitHub CLI\\gh.exe",
    `${process.env.LOCALAPPDATA}\\Programs\\GitHub CLI\\gh.exe`,
    `${process.env.USERPROFILE}\\scoop\\shims\\gh.exe`,
  ];

  for (const p of winPaths) {
    if (existsSync(p)) return `"${p}"`;
  }
  return "gh";
}

const ghCmd = getGhCommand();

process.env.GITHUB_PERSONAL_ACCESS_TOKEN = execSync(`${ghCmd} auth token`, {
  encoding: "utf8",
}).trim();

const npxCmd = process.platform === "win32" ? "npx.cmd" : "npx";
spawn(npxCmd, ["-y", "@modelcontextprotocol/server-github"], {
  stdio: "inherit",
  shell: true,
});
