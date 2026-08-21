import { AbilityConstant, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { deviceInfo } from '@kit.BasicServicesKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { WineWindowManager } from '../service/WineWindowManager';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;

interface D3dEnvironmentEntry {
  key: string;
  value: string;
}

interface ExperimentArtifactEntry {
  name: string;
  sha256: string;
}

export default class EntryAbility extends UIAbility {
  private contentPage: string = 'pages/Index';

  private defaultDxvkBackend(): string {
    /* This is an evidence allowlist, not a Host-to-Guest capability inference.
     * The bundled Guest Mesa/DXVK payload is fixed by the HAP manifest, while
     * the product/build identity below pins the audited Host driver. A system
     * update therefore falls back to 1.10.3 until that identity is qualified. */
    const qualified920 = deviceInfo.productModel === 'VYG-AL00' &&
      deviceInfo.incrementalVersion === '26.0.0.32';
    return qualified920 ? 'dxvk_modern_2_6' : 'dxvk_legacy';
  }

  private resolveDxvkBackend(value: string): string {
    if (value === 'dxvk_legacy' || value === 'dxvk_modern_2_6') return value;
    return this.defaultDxvkBackend();
  }

  private publishLaunchRequest(want: Want): boolean {
    const parameters = want.parameters;
    const value = (key: string, fallback: string): string => {
      if (!parameters || parameters[key] === undefined || parameters[key] === null) return fallback;
      return `${parameters[key]}`;
    };
    const mode = value('winehua.mode', '');
    const suite = value('winehua.suite', 'core');
    const explicitD3d = !!parameters && parameters['winehua.d3d_backend'] !== undefined &&
      parameters['winehua.d3d_backend'] !== null;
    const persistedD3dRaw = AppStorage.get<string>('winehua.d3d.preference') || 'vkd3d_limited_500k';
    const useProductD3dDefault = mode === '' || mode === 'game';
    const requestedD3d = explicitD3d ? value('winehua.d3d_backend', 'wined3d') :
      (useProductD3dDefault ? persistedD3dRaw : 'wined3d');
    const d3dBackend = requestedD3d === 'dxvk_legacy' ||
      requestedD3d === 'dxvk_modern_2_6' || requestedD3d === 'vkd3d_limited_500k' ?
      requestedD3d : 'wined3d';
    const explicitDxvk = !!parameters && parameters['winehua.dxvk_backend'] !== undefined &&
      parameters['winehua.dxvk_backend'] !== null;
    const persistedDxvk = AppStorage.get<string>('winehua.dxvk.preference') || 'auto';
    let dxvkBackend = this.resolveDxvkBackend(explicitDxvk ?
      value('winehua.dxvk_backend', 'auto') : persistedDxvk);
    if (d3dBackend === 'dxvk_legacy') dxvkBackend = 'dxvk_legacy';
    if (d3dBackend === 'dxvk_modern_2_6') dxvkBackend = 'dxvk_modern_2_6';
    // game_path 可能含 %20 等 URI 编码 (aa start 传参), 解码后再传给原生侧
    const rawGamePath = value('winehua.game_path', '');
    let gamePath = rawGamePath;
    if (rawGamePath.indexOf('%') >= 0) {
      try {
        gamePath = decodeURIComponent(rawGamePath);
      } catch (_err) {
        gamePath = rawGamePath;
      }
    }
    hilog.info(DOMAIN, 'GameTest', 'want game_path raw=%{public}s decoded=%{public}s',
      rawGamePath, gamePath);
    const experimentId = value('winehua.experiment_id', '');
    const experimentExecutable = value('winehua.experiment_executable', '');
    const experimentResultName = value('winehua.experiment_result_name', '');
    const experimentSourceUrl = value('winehua.experiment_source_url', '');
    const experimentNames: string[] = [];
    const experimentHashes: string[] = [];
    if (mode === 'experiment') {
      try {
        const entries = JSON.parse(decodeURIComponent(
          value('winehua.experiment_manifest_json', ''))) as ExperimentArtifactEntry[];
        if (Array.isArray(entries)) {
          for (const entry of entries) {
            if (experimentNames.length >= 16) break;
            if (entry && typeof entry.name === 'string' && typeof entry.sha256 === 'string') {
              experimentNames.push(entry.name);
              experimentHashes.push(entry.sha256);
            }
          }
        }
      } catch (_) {
        hilog.warn(DOMAIN, 'Experiment', 'invalid experiment artifact manifest');
      }
      // The native importer validates the components again before it touches
      // the prefix. Construct this path only for the isolated experiment tree.
      gamePath = `C:\\smoke\\experiments\\${experimentId}\\${experimentExecutable}`;
    }
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
      const allowedKey = key === 'VKD3D_DEBUG' ||
        /^(WINEDEBUG|DXVK_|VKD3D_TEST_|VN_|VKR_|WINEHUA_DXVK_|WINEHUA_VKR_)[A-Za-z0-9_]*$/.test(key) ||
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
      d3dBackend === 'vkd3d_limited_500k' ? 'shadow-precise' :
        (mode === '' || d3dBackend === 'dxvk_legacy' || d3dBackend === 'dxvk_modern_2_6' ?
          'shadow-precise-dirty-ring-inline-upload-coverage-sort' : 'baseline'));
    const perfProfile = ['direct-fence-wait', 'no-remote-sync', 'no-dynamic-flush', 'fence-feedback',
      'shadow-none', 'shadow-trace', 'shadow-to-host-explicit', 'shadow-precise',
      'shadow-precise-no-semaphore-feedback',
      'shadow-precise-no-semaphore-feedback-single-ring',
      'shadow-precise-no-semaphore-feedback-single-ring-sync-submit',
      'shadow-precise-no-semaphore-feedback-single-ring-readback-idle',
      'shadow-precise-no-semaphore-feedback-single-ring-trace',
      'shadow-full-no-semaphore-feedback-single-ring-trace',
      'shadow-precise-single-ring',
      'shadow-precise-sync-submit', 'shadow-precise-strong-ring',
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
      'shadow-precise-dirty-ring-bgra-array-trace',
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
    const request = JSON.stringify({
      mode: mode,
      runId: value('winehua.run_id', `${Date.now()}`),
      suite: suite,
      prefixMode: value('winehua.prefix', 'reuse'),
      longSeconds: Number(value('winehua.long_seconds', '3600')),
      batchMappedFlush: value('winehua.batch_mapped_flush', '0') === '1',
      experimentId: experimentId,
      experimentExecutable: experimentExecutable,
      experimentResultName: experimentResultName,
      experimentSourceUrl: experimentSourceUrl
    });
    AppStorage.setOrCreate<string>('winehua.smoke.request', request);
    AppStorage.set<string>('winehua.smoke.request', request);
    AppStorage.setOrCreate<string>('winehua.d3d.backend', d3dBackend);
    AppStorage.set<string>('winehua.d3d.backend', d3dBackend);
    AppStorage.setOrCreate<string>('winehua.dxvk.backend', dxvkBackend);
    AppStorage.set<string>('winehua.dxvk.backend', dxvkBackend);
    AppStorage.setOrCreate<string>('winehua.game.path', gamePath);
    AppStorage.set<string>('winehua.game.path', gamePath);
    AppStorage.setOrCreate<string[]>('winehua.experiment.names', experimentNames);
    AppStorage.set<string[]>('winehua.experiment.names', experimentNames);
    AppStorage.setOrCreate<string[]>('winehua.experiment.hashes', experimentHashes);
    AppStorage.set<string[]>('winehua.experiment.hashes', experimentHashes);
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
    // Publish this commit marker only after every field in the launch snapshot
    // has been updated. Index watches it to consume warm singleton Wants.
    const launchGeneration = (AppStorage.get<number>('winehua.launch.generation') || 0) + 1;
    AppStorage.setOrCreate<number>('winehua.launch.generation', launchGeneration);
    AppStorage.set<number>('winehua.launch.generation', launchGeneration);
    hilog.info(DOMAIN, 'SmokeRunner',
      'launch generation=%{public}d mode=%{public}s request=%{public}s',
      launchGeneration, mode, request);
    return mode === 'smoke' || mode === 'experiment';
  }

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    PersistentStorage.persistProp<string>('winehua.d3d.preference', 'vkd3d_limited_500k');
    PersistentStorage.persistProp<string>('winehua.dxvk.preference', 'auto');
    // 注: Wine 语言偏好不走 PersistentStorage (冷启动不恢复磁盘值, 实测),
    // 由 WineEnvService.init 用 preferences API 自行加载
    try {
      this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_DARK);
    } catch (err) {
      hilog.error(DOMAIN, 'testTag', 'Failed to set colorMode. Cause: %{public}s', JSON.stringify(err));
    }
    const automationMode = this.publishLaunchRequest(want);
    // Window ownership is a cold-session decision. A singleton onNewWant is
    // validated by Index before it may affect the running desktop session.
    WineWindowManager.getInstance().setAutomationMode(automationMode);
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onCreate');
    this.dumpEl1BundleLibs();
  }

  // [EL1-DUMP] 调试用: 验证 el1 bundle libs 目录实际内容 (定位 dlopen libEGL ENOENT)。
  // 结果写文件 (el2 files, hdc 可读) + hilog 双通道; 避免 hilog 抓取不可靠。
  private dumpEl1BundleLibs(): void {
    const dirs = ['/data/storage/el1/bundle/libs/arm64', '/data/storage/el1/bundle/libs'];
    const lines: string[] = [];
    for (const dir of dirs) {
      try {
        const names: string[] = fs.listFileSync(dir);
        const msg = `readdir ${dir} -> ${names.length} entries: ${names.join(',')}`;
        lines.push(msg);
        hilog.info(DOMAIN, 'EL1-DUMP', '%{public}s', msg);
      } catch (err) {
        const msg = `readdir ${dir} FAILED: ${JSON.stringify(err)}`;
        lines.push(msg);
        hilog.info(DOMAIN, 'EL1-DUMP', '%{public}s', msg);
      }
    }
    try {
      const dumpPath = this.context.filesDir + '/el1_dump.txt';
      const file = fs.openSync(dumpPath, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      fs.writeSync(file.fd, lines.join('\n'));
      fs.closeSync(file);
      hilog.info(DOMAIN, 'EL1-DUMP', 'written to %{public}s', dumpPath);
    } catch (err) {
      hilog.info(DOMAIN, 'EL1-DUMP', 'write file FAILED: %{public}s', JSON.stringify(err));
    }
  }

  onNewWant(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    this.publishLaunchRequest(want);
    WineWindowManager.getInstance().restoreDesktopIfRunning();
  }

  onDestroy(): void {
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    // Main window is created, set main page for this ability
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageCreate');

    // 手机走 fork-shim，必须在 Index.doInit 的首次 NCP 调用前设置。
    testNapi.setPhoneMode(deviceInfo.deviceType === 'phone');

    // Phone desktop is rendered in a full-window XComponent. Let the system
    // rotate that window with the sensor; its surface callback propagates the
    // resulting size to Wine before the next frame is composited.
    if (deviceInfo.deviceType === 'phone') {
      try {
        const mainWindow = windowStage.getMainWindowSync();
        mainWindow.setPreferredOrientation(window.Orientation.AUTO_ROTATION).then(() => {
          hilog.info(DOMAIN, 'testTag', 'phone auto-rotation enabled');
        }).catch((err: Error) => {
          hilog.warn(DOMAIN, 'testTag', 'phone auto-rotation failed: %{public}s', JSON.stringify(err));
        });
      } catch (err) {
        hilog.warn(DOMAIN, 'testTag', 'phone auto-rotation setup failed: %{public}s', JSON.stringify(err));
      }
    }

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
