import { spawnSync } from 'child_process';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';

import { hapTasks, registryCommandModifier, CommandBuilderType } from '@ohos/hvigor-ohos-plugin';

function resolveBuildJobs() {
  const candidates = [
    process.env.HVIGOR_JOBS,
    process.env.BUILD_JOBS,
    process.env.JOBS,
    process.env.NUMBER_OF_PROCESSORS
  ];

  for (const candidate of candidates) {
    const parsed = Number.parseInt(candidate ?? '', 10);
    if (Number.isFinite(parsed) && parsed > 0) {
      return parsed;
    }
  }

  return Math.max(1, os.cpus().length);
}

const buildJobs = resolveBuildJobs();
registryCommandModifier(CommandBuilderType.NINJA, {
  modify(command) {
    if (buildJobs < 2 || command.includes('-j')) {
      return command;
    }
    return [...command, '-j', String(buildJobs)];
  }
});

function resolveHapPath(entryDir, targetName, signed) {
  const suffix = signed ? 'signed' : 'unsigned';
  return path.join(entryDir, 'build', targetName, 'outputs', targetName, `entry-${targetName}-${suffix}.hap`);
}

function runHnpInjection(entryDir, hapPath, verifyOnly) {
  const scriptPath = path.join(entryDir, '..', 'scripts', 'inject_hnp_into_hap.ps1');
  if (!fs.existsSync(scriptPath) || !fs.existsSync(hapPath)) {
    return;
  }

  const args = [
    '-NoProfile',
    '-ExecutionPolicy',
    'Bypass',
    '-File',
    scriptPath,
    '-HapPath',
    hapPath,
    '-EntryDir',
    entryDir
  ];

  if (verifyOnly) {
    args.push('-VerifyOnly');
  }

  const result = spawnSync('powershell.exe', args, {
    cwd: entryDir,
    stdio: 'inherit'
  });

  if (result.status !== 0) {
    throw new Error(`inject_hnp_into_hap.ps1 failed for ${hapPath}`);
  }
}

function ensureSignedHapCarriesHnp(entryDir, targetName) {
  const scriptPath = path.join(entryDir, '..', 'scripts', 'ensure_hnp_signed_hap.ps1');
  const unsignedHapPath = resolveHapPath(entryDir, targetName, false);
  const signedHapPath = resolveHapPath(entryDir, targetName, true);
  if (!fs.existsSync(scriptPath) || !fs.existsSync(unsignedHapPath) || !fs.existsSync(signedHapPath)) {
    return;
  }

  const result = spawnSync('powershell.exe', [
    '-NoProfile',
    '-ExecutionPolicy',
    'Bypass',
    '-File',
    scriptPath,
    '-UnsignedHapPath',
    unsignedHapPath,
    '-SignedHapPath',
    signedHapPath,
    '-EntryDir',
    entryDir
  ], {
    cwd: entryDir,
    stdio: 'inherit'
  });

  if (result.status !== 0) {
    throw new Error(`ensure_hnp_signed_hap.ps1 failed for ${signedHapPath}`);
  }
}

function registerHnpRepairTasks(node, targetName) {
  const entryDir = node.getNodePath();
  node.registerTask({
    name: `winehuaInjectHnp_${targetName}`,
    dependencies: [`${targetName}@PackageHap`],
    postDependencies: [`${targetName}@SignHap`],
    run() {
      runHnpInjection(entryDir, resolveHapPath(entryDir, targetName, false), false);
    }
  });

  node.registerTask({
    name: `winehuaEnsureSignedHnp_${targetName}`,
    dependencies: [`${targetName}@SignHap`],
    postDependencies: [`${targetName}@CollectDebugSymbol`, 'assembleHap'],
    run() {
      ensureSignedHapCarriesHnp(entryDir, targetName);
    }
  });
}

const winehuaBuildPlugin = {
  pluginId: 'winehua-build',
  apply(node) {
    registerHnpRepairTasks(node, 'default');
  }
};

export default {
  system: hapTasks,
  plugins: [winehuaBuildPlugin]
};
