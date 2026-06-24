"use strict";

// Prepare the SDK overlay and toolchain environment when DevEco / hvigor
// runs directly on Windows without going through rebuild_harmony.ps1.

const fs = require("fs");
const path = require("path");

function exists(targetPath) {
  try {
    return fs.existsSync(targetPath);
  } catch {
    return false;
  }
}

function readJson(filePath) {
  return JSON.parse(fs.readFileSync(filePath, "utf8"));
}

function firstExisting(candidates) {
  for (const candidate of candidates) {
    if (candidate && exists(candidate)) {
      return candidate;
    }
  }
  return "";
}

function deriveDevEcoHome() {
  const envCandidates = [
    process.env.DEVECO_HOME,
    process.env.DEVECO_STUDIO_HOME
  ];
  const fromEnv = firstExisting(envCandidates);
  if (fromEnv) {
    return fromEnv;
  }

  const execPath = process.execPath || "";
  const normalizedExecPath = execPath.toLowerCase();
  const marker = `${path.sep}tools${path.sep}node${path.sep}node.exe`;
  if (normalizedExecPath.endsWith(marker.toLowerCase())) {
    return path.resolve(execPath, "..", "..", "..");
  }

  return firstExisting([
    "C:\\Program Files\\Huawei\\DevEco Studio",
    "D:\\Program Files\\Huawei\\DevEco Studio"
  ]);
}

function sameFileContent(leftPath, rightPath) {
  if (!exists(leftPath) || !exists(rightPath)) {
    return false;
  }
  return fs.readFileSync(leftPath).equals(fs.readFileSync(rightPath));
}

function ensureFileCopy(sourcePath, destPath) {
  if (sameFileContent(sourcePath, destPath)) {
    return;
  }
  fs.mkdirSync(path.dirname(destPath), { recursive: true });
  fs.copyFileSync(sourcePath, destPath);
}

function ensureDirectoryJunction(sourcePath, destPath) {
  if (exists(destPath)) {
    return;
  }
  fs.mkdirSync(path.dirname(destPath), { recursive: true });
  fs.symlinkSync(sourcePath, destPath, "junction");
}

function prependPath(dirPath) {
  if (!dirPath || !exists(dirPath)) {
    return;
  }

  const currentValue = process.env.PATH || process.env.Path || "";
  const parts = currentValue.split(path.delimiter).filter(Boolean);
  const hasDir = parts.some((part) => part.toLowerCase() === dirPath.toLowerCase());
  if (hasDir) {
    return;
  }

  const nextValue = [dirPath, ...parts].join(path.delimiter);
  process.env.PATH = nextValue;
  process.env.Path = nextValue;
}

function ensureDevEcoEnvironment(repoRoot) {
  if (process.platform !== "win32") {
    return;
  }

  const devEcoHome = deriveDevEcoHome();
  if (!devEcoHome) {
    return;
  }

  const sdkDefaultRoot = path.join(devEcoHome, "sdk", "default");
  const sdkPkgPath = path.join(sdkDefaultRoot, "sdk-pkg.json");
  if (!exists(sdkPkgPath)) {
    return;
  }

  const sdkPkg = readJson(sdkPkgPath);
  const versionDirName = sdkPkg && sdkPkg.data && sdkPkg.data.path;
  if (!versionDirName) {
    return;
  }

  const overlayRoot = path.join(repoRoot, "out", "sdk-links", "harmonyos-sdk-root");
  const versionRoot = path.join(overlayRoot, versionDirName);
  fs.mkdirSync(versionRoot, { recursive: true });

  ensureFileCopy(sdkPkgPath, path.join(versionRoot, "sdk-pkg.json"));
  ensureDirectoryJunction(path.join(sdkDefaultRoot, "openharmony"), path.join(versionRoot, "openharmony"));
  ensureDirectoryJunction(path.join(sdkDefaultRoot, "hms"), path.join(versionRoot, "hms"));

  const ohosBaseSdkHome = path.join(versionRoot, "openharmony");
  process.env.DEVECO_HOME = devEcoHome;
  process.env.DEVECO_SDK_HOME = overlayRoot;
  process.env.OHOS_BASE_SDK_HOME = ohosBaseSdkHome;
  process.env.HOS_SDK_HOME = ohosBaseSdkHome;

  const nodeHome = path.join(devEcoHome, "tools", "node");
  if (exists(path.join(nodeHome, "node.exe"))) {
    process.env.NODE_HOME = nodeHome;
    prependPath(nodeHome);
  }

  const javaHome = path.join(devEcoHome, "jbr");
  const javaBin = path.join(javaHome, "bin");
  if (exists(path.join(javaBin, "java.exe"))) {
    process.env.JAVA_HOME = javaHome;
    prependPath(javaBin);
  }
}

module.exports = {
  ensureDevEcoEnvironment
};
