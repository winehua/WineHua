import { hilog } from '@kit.PerformanceAnalysisKit';
import { WineEnvService } from '../service/WineEnvService';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;
const TAG = 'GameTest';

interface D3dEnvironmentEntry {
  key: string;
  value: string;
}

interface GameRequest {
  path: string;
  argv: string[];
  d3dEnvKeys: string[];
  d3dEnvValues: string[];
  clickTitlePrefix: string;
  clickButtonText: string;
  clickDelayMs: number;
  clickClientXPermille: number;
  clickClientYPermille: number;
}

// Want mode=game automation (Heaven / hdc aa start). Mirrors SmokeHook isolation:
// product files only keep marked one-line hooks. URI-decode paths so spaces work.
export class GameHook {
  private static instance: GameHook | null = null;

  static getInstance(): GameHook {
    if (!GameHook.instance) {
      GameHook.instance = new GameHook();
    }
    return GameHook.instance;
  }

  private env: WineEnvService | null = null;
  private pending: GameRequest | null = null;
  private started: boolean = false;
  private active: boolean = false;

  isActive(): boolean {
    return this.active;
  }

  applyWant(parameters: Record<string, Object> | undefined): void {
    const value = (key: string, fallback: string): string => {
      if (!parameters || parameters[key] === undefined || parameters[key] === null) return fallback;
      return `${parameters[key]}`;
    };
    if (value('winehua.mode', '') !== 'game') return;

    const rawGamePath = value('winehua.game_path', '');
    let gamePath = rawGamePath;
    if (rawGamePath.indexOf('%') >= 0) {
      try {
        gamePath = decodeURIComponent(rawGamePath);
      } catch (_err) {
        gamePath = rawGamePath;
      }
    }
    hilog.info(DOMAIN, TAG, 'want game_path raw=%{public}s decoded=%{public}s',
      rawGamePath, gamePath);

    const requestedGameArgCount = Number(value('winehua.game_argc', '0'));
    const gameArgCount = Math.max(0, Math.min(64, Math.floor(requestedGameArgCount)));
    const gameArguments: string[] = [];
    const encodedArguments = value('winehua.game_args_json', '');
    let decodedArguments = false;
    if (encodedArguments.length > 0) {
      try {
        const parsed: string[] = JSON.parse(decodeURIComponent(encodedArguments)) as string[];
        if (Array.isArray(parsed)) {
          for (const item of parsed) {
            if (gameArguments.length < gameArgCount) {
              gameArguments.push(item);
            }
          }
          decodedArguments = gameArguments.length === gameArgCount;
        }
      } catch (_err) {
        hilog.warn(DOMAIN, TAG, 'invalid encoded game argv JSON');
      }
    }
    if (!decodedArguments) {
      gameArguments.length = 0;
      for (let index = 0; index < gameArgCount; ++index) {
        const encoded = value(`winehua.game_arg_enc${index}`, '');
        if (encoded.length > 0) {
          try {
            gameArguments.push(decodeURIComponent(encoded));
            continue;
          } catch (_err) {
            hilog.warn(DOMAIN, TAG, 'invalid encoded game argument index=%{public}d', index);
          }
        }
        gameArguments.push(value(`winehua.game_arg${index}`, ''));
      }
    }

    const d3dEnvKeys: string[] = [];
    const d3dEnvValues: string[] = [];
    const appendD3dEnvironment = (key: string, envValue: string): void => {
      const allowedBox64Keys = [
        'BOX64_DYNAREC_SAFEFLAGS',
        'BOX64_DYNAREC_BIGBLOCK',
        'BOX64_DYNAREC_CALLRET',
        'BOX64_DYNAREC_FORWARD',
        'BOX64_DYNAREC_STRONGMEM',
        'BOX64_AVX'
      ];
      const allowedKey = /^(WINEDEBUG|DXVK_|VN_|VKR_|WINEHUA_DXVK_|WINEHUA_VKR_)[A-Za-z0-9_]*$/.test(key) ||
        allowedBox64Keys.indexOf(key) >= 0;
      if (!allowedKey || envValue.length > 1024 || d3dEnvKeys.length >= 32) {
        hilog.warn(DOMAIN, TAG, 'ignoring invalid D3D environment key=%{public}s', key);
        return;
      }
      d3dEnvKeys.push(key);
      d3dEnvValues.push(envValue);
    };
    const encodedD3dEnvironment = value('winehua.d3d_env_json', '');
    let decodedD3dEnvironment = false;
    if (encodedD3dEnvironment.length > 0) {
      try {
        const parsed: D3dEnvironmentEntry[] =
          JSON.parse(decodeURIComponent(encodedD3dEnvironment)) as D3dEnvironmentEntry[];
        if (Array.isArray(parsed)) {
          for (const entry of parsed) {
            if (entry && typeof entry.key === 'string' && typeof entry.value === 'string') {
              appendD3dEnvironment(entry.key, entry.value);
            }
          }
          decodedD3dEnvironment = true;
        }
      } catch (_err) {
        hilog.warn(DOMAIN, TAG, 'invalid encoded D3D environment JSON');
      }
    }
    if (!decodedD3dEnvironment) {
      const requestedD3dEnvCount = Number(value('winehua.d3d_env_count', '0'));
      const d3dEnvCount = Math.max(0, Math.min(32, Math.floor(requestedD3dEnvCount)));
      for (let index = 0; index < d3dEnvCount; ++index) {
        appendD3dEnvironment(value(`winehua.d3d_env_key${index}`, ''),
          value(`winehua.d3d_env_value${index}`, ''));
      }
    }

    const requestedClickDelayMs = Number(value('winehua.click_delay_ms', '1500'));
    const requestedClickClientXPermille = Number(value('winehua.click_client_x_permille', '-1'));
    const requestedClickClientYPermille = Number(value('winehua.click_client_y_permille', '-1'));

    this.pending = {
      path: gamePath,
      argv: gameArguments,
      d3dEnvKeys: d3dEnvKeys,
      d3dEnvValues: d3dEnvValues,
      clickTitlePrefix: value('winehua.click_title_prefix', ''),
      clickButtonText: value('winehua.click_button_text', ''),
      clickDelayMs: Math.max(0, Math.min(30000, Math.floor(requestedClickDelayMs))),
      clickClientXPermille: Math.max(-1, Math.min(1000, Math.floor(requestedClickClientXPermille))),
      clickClientYPermille: Math.max(-1, Math.min(1000, Math.floor(requestedClickClientYPermille)))
    };
    this.started = false;
    this.active = gamePath.length > 0;
    hilog.info(DOMAIN, TAG, 'game want armed path=%{public}s argc=%{public}d env=%{public}d',
      gamePath, gameArguments.length, d3dEnvKeys.length);
  }

  attach(env: WineEnvService): void {
    this.env = env;
    this.runPending();
  }

  onNewWant(): void {
    this.runPending();
  }

  onEngineReady(): void {
    this.runPending();
  }

  private windowsParentDirectory(windowsPath: string): string {
    const normalized = windowsPath.replace(/\//g, '\\');
    const idx = normalized.lastIndexOf('\\');
    if (idx <= 0) return 'C:\\';
    return normalized.substring(0, idx);
  }

  private buildEnvironment(req: GameRequest): Record<string, string> {
    // Product DXVK_LOG / WINEDEBUG come from native AppendStableDxvkEnv
    // (same as Index.doRun). Want may still overlay explicit D3D keys.
    const environment: Record<string, string> = {};
    const n = Math.min(req.d3dEnvKeys.length, req.d3dEnvValues.length);
    for (let i = 0; i < n; ++i) {
      environment[req.d3dEnvKeys[i]] = req.d3dEnvValues[i];
    }
    return environment;
  }

  private runPending(): void {
    const req = this.pending;
    const env = this.env;
    if (!req || !env || this.started) return;
    if (!env.prefixReady || env.engineState !== 'ready') {
      hilog.info(DOMAIN, TAG,
        'game launch deferred path=%{public}s prefixReady=%{public}d engine=%{public}s',
        req.path, env.prefixReady ? 1 : 0, env.engineState);
      return;
    }
    if (req.path.length === 0) {
      hilog.error(DOMAIN, TAG, 'game want armed but path is empty');
      return;
    }
    this.started = true;
    const environment = this.buildEnvironment(req);
    const d3dBackend = env.getD3dBackend();
    const dxvkBackend = env.getDxvkBackend();
    hilog.info(DOMAIN, TAG, 'launching %{public}s argc=%{public}d d3d=%{public}s dxvk=%{public}s',
      req.path, req.argv.length, d3dBackend, dxvkBackend);
    const launched = testNapi.runWineProgram({
      windowsExePath: req.path,
      argv: req.argv.slice(),
      environment: environment,
      workingDirectory: this.windowsParentDirectory(req.path),
      d3dBackend: d3dBackend,
      dxvkBackend: dxvkBackend
    }) as Record<string, Object>;
    const pid = Number(launched['pid'] ?? 0);
    hilog.info(DOMAIN, TAG, 'game launch done path=%{public}s pid=%{public}d status=%{public}s',
      req.path, pid, `${launched['status'] ?? ''}`);
    if (!(pid > 0)) {
      hilog.error(DOMAIN, TAG, 'game spawn failed path=%{public}s', req.path);
      return;
    }

    if (req.clickTitlePrefix.length > 0 && req.clickButtonText.length > 0) {
      const titlePrefix = req.clickTitlePrefix;
      const buttonText = req.clickButtonText;
      const delayMs = req.clickDelayMs;
      const clickXPermille = req.clickClientXPermille;
      const clickYPermille = req.clickClientYPermille;
      setTimeout(() => {
        const driverArguments = ['--attach', '--title-prefix', titlePrefix,
          '--button-text', buttonText, '--timeout-ms', '60000'];
        if (clickXPermille >= 0 && clickYPermille >= 0) {
          driverArguments.push('--client-x-permille', `${clickXPermille}`,
            '--client-y-permille', `${clickYPermille}`);
        }
        testNapi.runWineProgram({
          windowsExePath: 'C:\\smoke\\x64\\winehua_win32_driver.exe',
          argv: driverArguments,
          environment: {},
          workingDirectory: 'C:\\smoke\\x64',
          d3dBackend: d3dBackend,
          dxvkBackend: dxvkBackend
        });
      }, delayMs);
    }
  }
}
