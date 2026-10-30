export interface GameEntry {
  id: string;
  title: string;
  directory: string;
  executables: string[];
  suggestedExecutable: string;
}

export interface GameProfile {
  id: string;
  name: string;
  executable: string;
  argumentsText: string;
  workingDirectory: string;
  d3dBackend: string;
  openglRenderer: string;
  compatMode: string;
  resolution: string;
  environmentText: string;
  lastLaunchedAt: number;
}

export interface GameLaunchResult {
  accepted: boolean;
  requiresRestart: boolean;
  message: string;
  pid: number;
}

export const GAME_BACKENDS: string[] = [
  'inherit', 'wined3d', 'dxvk_legacy', 'dxvk_legacy_d3d9', 'dxvk_modern_2_6', 'vkd3d_limited_500k'
];
export const GAME_COMPAT_MODES: string[] = ['', 'stability', 'conservative', 'intermediate', 'performance'];
export const GAME_OPENGL_RENDERERS: string[] = ['auto', 'virpipe', 'zink'];
export const GAME_RESOLUTIONS: string[] = ['inherit', 'auto', '1024x768'];

export function relativeGamePath(value: string): string {
  const path = value.replace(/\\/g, '/');
  if (path.startsWith('/') || path.indexOf(':') >= 0 || /[|\0\r\n]/.test(path) ||
      path.split('/').some(part => part === '..')) throw new Error('路径必须位于 games 目录内');
  return path.split('/').filter(part => part !== '' && part !== '.').join('/');
}

export function executableScore(path: string, directory: string): number {
  const parts = path.split('/');
  const file = parts[parts.length - 1].toLowerCase();
  if (/^(unins.*|uninstall.*|setup.*|install.*|dxsetup|vcredist.*|vc_redist.*|unitycrashhandler.*|crashreport.*|crashpad.*|.*updater|steam_workshop_upload|localization|savegametransfer)\.exe$/.test(file) ||
      /(^|\/)(_?commonredist|redist|redistributables|prerequisites)(\/|$)/i.test(path)) return -1000;
  const stem = file.slice(0, -4).replace(/[\s_\-]/g, '');
  const folder = directory.split('/').pop() || '';
  let score = 100 - parts.length * 8;
  if (stem === folder.toLowerCase().replace(/[\s_\-]/g, '')) score += 100;
  else if (stem.length >= 5 && folder.toLowerCase().replace(/[\s_\-]/g, '').startsWith(stem)) score += 60;
  if (/^(启动器|开始游戏|启动游戏|game|launcher|start|main)$/.test(stem)) score += 70;
  if (/\/binaries\/win64\//i.test(path)) score += 15;
  else if (/\/(win64|x64)\//i.test(path)) score += 10;
  return score;
}

// Each top-level game folder is one entry; all nested EXEs remain selectable.
// Executables directly in games are separate entries. Empty folders stay visible.
export function buildGameEntries(files: string[], directories: string[]): GameEntry[] {
  const groups = new Map<string, GameEntry>();
  for (const name of directories) {
    const directory = relativeGamePath(name);
    if (!directory || directory.indexOf('/') >= 0) continue;
    const id = 'folder:' + directory;
    groups.set(id, { id, title: directory, directory, executables: [], suggestedExecutable: '' });
  }
  for (const file of files) {
    const path = relativeGamePath(file);
    if (!/\.exe$/i.test(path)) continue;
    const slash = path.indexOf('/');
    const directory = slash < 0 ? '' : path.substring(0, slash);
    const id = slash < 0 ? 'file:' + path : 'folder:' + directory;
    let game = groups.get(id);
    if (!game) {
      game = { id, title: directory || path.replace(/\.exe$/i, ''), directory,
        executables: [], suggestedExecutable: '' };
      groups.set(id, game);
    }
    if (game.executables.indexOf(path) < 0) game.executables.push(path);
  }
  const games = Array.from(groups.values());
  for (const game of games) {
    game.executables.sort((a, b) => executableScore(b, game.directory) - executableScore(a, game.directory) || a.localeCompare(b));
    if (game.executables.length && executableScore(game.executables[0], game.directory) > -1000)
      game.suggestedExecutable = game.executables[0];
  }
  return games.sort((a, b) => a.title.localeCompare(b.title));
}

export function defaultGameProfile(game: GameEntry): GameProfile {
  return { id: game.id, name: game.title, executable: game.suggestedExecutable,
    argumentsText: '', workingDirectory: '', d3dBackend: 'inherit', openglRenderer: 'auto', compatMode: '',
    resolution: 'inherit', environmentText: '', lastLaunchedAt: 0 };
}

export function expandGameEntries(games: GameEntry[]): GameEntry[] {
  const programs: GameEntry[] = [];
  for (const game of games) {
    for (const executable of game.executables) {
      const slash = executable.lastIndexOf('/');
      const stem = executable.substring(slash + 1).replace(/\.exe$/i, '');
      programs.push({ id: 'exe:' + executable,
        title: stem.toLowerCase() === game.title.toLowerCase() ? game.title : `${game.title} · ${stem}`,
        directory: slash >= 0 ? executable.substring(0, slash) : '', executables: [executable],
        suggestedExecutable: executableScore(executable, game.directory) > -1000 ? executable : '' });
    }
  }
  return programs.sort((a, b) => a.title.localeCompare(b.title));
}

export function normalizeGameProfile(profile: GameProfile, game: GameEntry): GameProfile {
  const defaults = defaultGameProfile(game);
  return { id: game.id, name: typeof profile.name === 'string' && profile.name.trim() ? profile.name.trim() : defaults.name,
    executable: typeof profile.executable === 'string' ? relativeGamePath(profile.executable) : defaults.executable,
    argumentsText: typeof profile.argumentsText === 'string' ? profile.argumentsText : '',
    workingDirectory: typeof profile.workingDirectory === 'string' ? relativeGamePath(profile.workingDirectory) : '',
    d3dBackend: GAME_BACKENDS.indexOf(profile.d3dBackend) >= 0 ? profile.d3dBackend : 'inherit',
    openglRenderer: GAME_OPENGL_RENDERERS.indexOf(profile.openglRenderer) >= 0 ? profile.openglRenderer : 'auto',
    compatMode: GAME_COMPAT_MODES.indexOf(profile.compatMode) >= 0 ? profile.compatMode : '',
    resolution: GAME_RESOLUTIONS.indexOf(profile.resolution) >= 0 ? profile.resolution : 'inherit',
    environmentText: typeof profile.environmentText === 'string' ? profile.environmentText : '',
    lastLaunchedAt: Number.isFinite(profile.lastLaunchedAt) && profile.lastLaunchedAt > 0 ? profile.lastLaunchedAt : 0 };
}

export function parseGameArguments(text: string): string[] {
  const result: string[] = [];
  let value = '', quoted = false, started = false, i = 0;
  while (i < text.length) {
    const c = text[i];
    if (!quoted && /\s/.test(c)) {
      if (started) { result.push(value); value = ''; started = false; }
      i++; continue;
    }
    started = true;
    if (c === '\\') {
      let count = 0;
      while (text[i] === '\\') { count++; i++; }
      if (text[i] === '"') {
        value += '\\'.repeat(Math.floor(count / 2));
        if (count % 2) value += '"'; else quoted = !quoted;
        i++;
      } else value += '\\'.repeat(count);
    } else if (c === '"') {
      if (quoted && text[i + 1] === '"') { value += '"'; i += 2; }
      else { quoted = !quoted; i++; }
    }
    else { value += c; i++; }
  }
  if (quoted) throw new Error('启动参数中的双引号未闭合');
  if (started) result.push(value);
  if (result.some(argument => /[|\0\r\n]/.test(argument))) throw new Error('启动参数包含当前启动协议不支持的字符');
  return result;
}

export function parseGameEnvironment(text: string): Record<string, string> {
  const result: Record<string, string> = {};
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    if (/[|\0]/.test(line)) throw new Error('环境变量包含当前启动协议不支持的字符');
    const separator = line.indexOf('=');
    if (separator <= 0) throw new Error('环境变量需使用 KEY=VALUE，每行一项');
    const key = line.substring(0, separator).trim();
    if (key === 'WINEDEBUG') throw new Error('日志覆盖请使用 WINEHUA_WINEDEBUG，避免被 Wine 启动策略覆盖');
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(key) || ['__proto__', 'constructor', 'prototype'].indexOf(key) >= 0)
      throw new Error('无效的环境变量名：' + key);
    if (['WINEPREFIX', 'WAYLAND_DISPLAY', 'XDG_RUNTIME_DIR', 'PROCESSBROKER', 'WINESERVERSOCKET'].indexOf(key) >= 0)
      throw new Error(key + ' 由 Wine 会话管理，不能按游戏覆盖');
    result[key] = line.substring(separator + 1);
  }
  return result;
}
