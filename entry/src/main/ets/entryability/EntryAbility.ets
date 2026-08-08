import { AbilityConstant, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { deviceInfo } from '@kit.BasicServicesKit';
import { WineWindowManager } from '../service/WineWindowManager';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;

interface D3dEnvironmentEntry {
  key: string;
  value: string;
}

export default class EntryAbility extends UIAbility {
  private contentPage: string = 'pages/Index';

  private publishLaunchRequest(want: Want): void {
    const parameters = want.parameters;
    const value = (key: string, fallback: string): string => {
      if (!parameters || parameters[key] === undefined || parameters[key] === null) return fallback;
      return `${parameters[key]}`;
    };
    const mode = value('winehua.mode', '');
    const suite = value('winehua.suite', 'core');
    const explicitD3d = !!parameters && parameters['winehua.d3d_backend'] !== undefined &&
      parameters['winehua.d3d_backend'] !== null;
    const persistedD3d = AppStorage.get<string>('winehua.d3d.preference') || 'dxvk_legacy';
    const requestedD3d = explicitD3d ? value('winehua.d3d_backend', 'wined3d') :
      (mode === '' ? persistedD3d : 'wined3d');
    const d3dBackend = requestedD3d === 'dxvk_legacy' ? 'dxvk_legacy' : 'wined3d';
    const gamePath = value('winehua.game_path', '');
    const requestedGameArgCount = Number(value('winehua.game_argc', '0'));
    const gameArgCount = Math.max(0, Math.min(64, Math.floor(requestedGameArgCount)));
    const gameArguments: string[] = [];
    // aa start has a finite Want-parameter budget and also interprets a raw
    // value beginning with '-' as an aa option.  The automation launcher
    // therefore sends the complete argv array as one URI-encoded JSON value.
    // Keep the per-item encoded/raw fields as a compatibility fallback.
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
      } catch (err) {
        hilog.warn(DOMAIN, 'GameTest', 'invalid encoded game argv JSON');
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
          } catch (err) {
            hilog.warn(DOMAIN, 'GameTest',
              'invalid encoded game argument index=%{public}d', index);
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
      if (!allowedKey ||
        envValue.length > 1024 || d3dEnvKeys.length >= 32) {
        hilog.warn(DOMAIN, 'GameTest',
          'ignoring invalid D3D environment key=%{public}s', key);
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
      } catch (err) {
        hilog.warn(DOMAIN, 'GameTest', 'invalid encoded D3D environment JSON');
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
    hilog.info(DOMAIN, 'GameTest', 'D3D environment decoded source=%{public}s count=%{public}d',
      decodedD3dEnvironment ? 'json' : 'legacy', d3dEnvKeys.length);
    const clickTitlePrefix = value('winehua.click_title_prefix', '');
    const clickButtonText = value('winehua.click_button_text', '');
    const requestedClickDelayMs = Number(value('winehua.click_delay_ms', '1500'));
    const clickDelayMs = Math.max(0, Math.min(30000, Math.floor(requestedClickDelayMs)));
    const requestedClickClientXPermille = Number(value('winehua.click_client_x_permille', '-1'));
    const requestedClickClientYPermille = Number(value('winehua.click_client_y_permille', '-1'));
    const clickClientXPermille = Math.max(-1,
      Math.min(1000, Math.floor(requestedClickClientXPermille)));
    const clickClientYPermille = Math.max(-1,
      Math.min(1000, Math.floor(requestedClickClientYPermille)));
    const requestedPerfProfile = value('winehua.perf_profile',
      mode === '' || d3dBackend === 'dxvk_legacy' ?
        'shadow-precise-dirty-ring-inline-upload-coverage-sort' : 'baseline');
    const perfProfile = ['direct-fence-wait', 'no-remote-sync', 'no-dynamic-flush', 'fence-feedback',
      'shadow-none', 'shadow-trace', 'shadow-to-host-explicit', 'shadow-precise',
      'shadow-precise-single-ring', 'shadow-precise-sync-submit', 'shadow-precise-strong-ring',
      'shadow-precise-legacy-host-sync',
      'shadow-precise-dirty-ring', 'shadow-precise-dirty-ring-perf', 'shadow-precise-dirty-ring-gpu-frame-profile', 'shadow-precise-dirty-ring-frame-timeline', 'shadow-precise-dirty-ring-no-merge', 'shadow-precise-dirty-ring-no-upload',
      'shadow-precise-dirty-ring-no-upload-fast',
      'shadow-precise-dirty-ring-inline-upload',
      'shadow-precise-dirty-ring-inline-upload-coverage-sort',
      'shadow-precise-dirty-ring-coverage-sort-sampled',
      'shadow-precise-dirty-ring-coverage-poll',
      'shadow-precise-dirty-ring-inline-upload-alias-cover',
      'shadow-precise-dirty-ring-inline-upload-serialized',
      'shadow-precise-dirty-ring-inline-upload-descriptor-serialized',
      'shadow-precise-dirty-ring-frame-assoc-trace',
      'shadow-precise-dirty-ring-present-image-trace',
      'shadow-precise-strong-ring-trace', 'shadow-precise-strong-ring-perf',
      'shadow-precise-strong-ring-async-present',
      'shadow-precise-strong-ring-fence-poll',
      'shadow-precise-strong-ring-mailbox',
      'shadow-precise-direct-fence', 'shadow-precise-retain-shmem',
      'shadow-precise-cpu-upload']
      .indexOf(requestedPerfProfile) >= 0 ? requestedPerfProfile : 'baseline';
    this.contentPage = mode === 'smoke' && suite === 'host-vulkan' ?
      'pages/HostVulkanSmoke' : 'pages/Index';
    WineWindowManager.getInstance().setAutomationMode(mode === 'smoke');
    const request = JSON.stringify({
      mode: mode,
      runId: value('winehua.run_id', `${Date.now()}`),
      suite: suite,
      prefixMode: value('winehua.prefix', 'reuse'),
      longSeconds: Number(value('winehua.long_seconds', '3600')),
      batchMappedFlush: value('winehua.batch_mapped_flush', '0') === '1'
    });
    AppStorage.setOrCreate<string>('winehua.smoke.request', request);
    AppStorage.set<string>('winehua.smoke.request', request);
    AppStorage.setOrCreate<string>('winehua.d3d.backend', d3dBackend);
    AppStorage.set<string>('winehua.d3d.backend', d3dBackend);
    AppStorage.setOrCreate<string>('winehua.game.path', gamePath);
    AppStorage.set<string>('winehua.game.path', gamePath);
    AppStorage.setOrCreate<string[]>('winehua.game.args', gameArguments);
    AppStorage.set<string[]>('winehua.game.args', gameArguments);
    AppStorage.setOrCreate<string[]>('winehua.game.d3d_env_keys', d3dEnvKeys);
    AppStorage.set<string[]>('winehua.game.d3d_env_keys', d3dEnvKeys);
    AppStorage.setOrCreate<string[]>('winehua.game.d3d_env_values', d3dEnvValues);
    AppStorage.set<string[]>('winehua.game.d3d_env_values', d3dEnvValues);
    AppStorage.setOrCreate<string>('winehua.game.click_title_prefix', clickTitlePrefix);
    AppStorage.set<string>('winehua.game.click_title_prefix', clickTitlePrefix);
    AppStorage.setOrCreate<string>('winehua.game.click_button_text', clickButtonText);
    AppStorage.set<string>('winehua.game.click_button_text', clickButtonText);
    AppStorage.setOrCreate<number>('winehua.game.click_delay_ms', clickDelayMs);
    AppStorage.set<number>('winehua.game.click_delay_ms', clickDelayMs);
    AppStorage.setOrCreate<number>('winehua.game.click_client_x_permille', clickClientXPermille);
    AppStorage.set<number>('winehua.game.click_client_x_permille', clickClientXPermille);
    AppStorage.setOrCreate<number>('winehua.game.click_client_y_permille', clickClientYPermille);
    AppStorage.set<number>('winehua.game.click_client_y_permille', clickClientYPermille);
    AppStorage.setOrCreate<string>('winehua.perf.profile', perfProfile);
    AppStorage.set<string>('winehua.perf.profile', perfProfile);
    hilog.info(DOMAIN, 'SmokeRunner', 'launch mode=%{public}s request=%{public}s', mode, request);
  }

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    PersistentStorage.persistProp<string>('winehua.d3d.preference', 'dxvk_legacy');
    // 注: Wine 语言偏好不走 PersistentStorage (冷启动不恢复磁盘值, 实测),
    // 由 WineEnvService.init 用 preferences API 自行加载
    try {
      this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_DARK);
    } catch (err) {
      hilog.error(DOMAIN, 'testTag', 'Failed to set colorMode. Cause: %{public}s', JSON.stringify(err));
    }
    this.publishLaunchRequest(want);
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onCreate');
  }

  onNewWant(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    this.publishLaunchRequest(want);
  }

  onDestroy(): void {
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    // Main window is created, set main page for this ability
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageCreate');

    // 手机走 fork-shim，必须在 Index.doInit 的首次 NCP 调用前设置。
    testNapi.setPhoneMode(deviceInfo.deviceType === 'phone');

    // 初始化多窗口管理器
    WineWindowManager.getInstance().init(windowStage, this.context);

    windowStage.loadContent(this.contentPage, (err) => {
      if (err.code) {
        hilog.error(DOMAIN, 'testTag', 'Failed to load the content. Cause: %{public}s', JSON.stringify(err));
        return;
      }
      // loadContent 成功后隐藏窗口装饰
      let mainWin = windowStage.getMainWindowSync();
      if (mainWin) {
        mainWin.setWindowDecorVisible(false);
      }
      hilog.info(DOMAIN, 'testTag', 'Succeeded in loading the content.');
    });
  }

  onWindowStageDestroy(): void {
    // Main window is destroyed, release UI related resources
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageDestroy');
  }

  onForeground(): void {
    // Ability has brought to foreground
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onForeground');
  }

  onBackground(): void {
    // Ability has back to background
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onBackground');
  }
}
