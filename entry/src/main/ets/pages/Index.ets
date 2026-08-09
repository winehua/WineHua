import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import { fileIo as fs } from '@kit.CoreFileKit';
import { WineEnvService } from '../service/WineEnvService';
import { ProcessService, ProcessInfo } from '../service/ProcessService';

const DOMAIN = 0x0000;

// 文件浏览器以 C: 盘为根 (与 WineEnvService 的 DRIVE_C 同值)
const DRIVE_C = '/data/storage/el2/base/files/.wine/drive_c';

@Entry
@Component
struct Index {
  @StorageLink('winehua.d3d.backend') d3dBackendText: string = 'dxvk_legacy';
  @StorageLink('winehua.d3d.preference') d3dPreferenceText: string = 'dxvk_legacy';
  @StorageLink('winehua.game.d3d_env_keys') d3dEnvKeys: string[] = [];
  @StorageLink('winehua.game.d3d_env_values') d3dEnvValues: string[] = [];
  @StorageLink('winehua.perf.profile') perfProfileText: string = 'baseline';
  private env = WineEnvService.getInstance();
  // -- 页面镜像 (WineEnvService notify 驱动) --
  @State state: string = 'idle';
  @State prefixReady: boolean = false;
  @State statusText: string = '检测中...';
  @State initOverlayVisible: boolean = false;
  @State initOverlayText: string = '';
  @State initFailed: boolean = false;
  @State initBusy: boolean = false;
  @State opBusy: boolean = false;
  @State upgradePending: boolean = false;
  // -- 文件浏览器 --
  @State curDir: string = DRIVE_C;
  @State entries: string[] = [];
  @State dirStack: string[] = [];
  // -- 运行中进程 (ProcessService 镜像) --
  @State processList: ProcessInfo[] = [];
  private d3dBackend: string = 'dxvk_legacy';
  // 升级提示日志只打一次 (master 版无弹卡 UI, 检测到不一致仅留痕 +
  // 快速操作按钮转为"应用更新"语义, 见 quickStart)
  private upgradeLogged: boolean = false;

  aboutToAppear(): void {
    this.d3dBackend = this.d3dBackendText === 'dxvk_legacy' ? 'dxvk_legacy' : 'wined3d';
    const env = this.env;
    const mirror = (): void => {
      this.state = env.engineState;
      this.prefixReady = env.prefixReady;
      this.statusText = env.statusText;
      this.initOverlayText = env.initOverlayText;
      this.initOverlayVisible = env.initOverlayVisible;
      this.initFailed = env.initFailed;
      this.initBusy = env.initBusy;
      this.opBusy = env.opBusy;
      this.upgradePending = env.upgradePending;
      if (env.upgradePending && !this.upgradeLogged) {
        this.upgradeLogged = true;
        hilog.warn(DOMAIN, 'wine',
          'upgrade pending: packaged wine runtime differs from extracted data. ' +
          'Use "重置 Wine" to apply the update (full factory reset).');
      }
    };
    env.subscribe(mirror);
    // 进程列表镜像 + 引擎事件驱动刷新 (WineEnvService 内部已绑定时序)
    const proc = ProcessService.getInstance();
    proc.subscribe(() => {
      this.processList = proc.processList;
    });
    proc.bindEnv(env);
    // 引擎初始化 (幂等): automation/game 模式自动 doInit; prefix 就绪自动
    // 拉起 (含升级检测拦截); 首启懒初始化等用户点"初始化 Wine"
    env.init(getContext(this));
    mirror();
  }

  private d3dLaunchEnvironment(): Record<string, string> {
    if (this.d3dBackend !== 'dxvk_legacy') return {};
    const environment: Record<string, string> = {
      'DXVK_LOG_LEVEL': 'warn',
      'DXVK_LOG_PATH': 'C:\\smoke\\results\\desktop-dxvk',
      'WINEDEBUG': '-all',
      /* Venus uses a shared-memory command ring between the x86_64 guest and
       * the ARM64 renderer.  Box64's aggressive weak-barrier mode can violate
       * the x86 ordering expected by the ring and eventually corrupt a random
       * Vulkan command. Keep this scoped to DXVK/Venus processes. */
      'BOX64_DYNAREC_WEAKBARRIER': '0',
      'WINEHUA_PERF_PROFILE': this.perfProfileText
    };
    if (this.perfProfileText === 'shadow-precise-strong-ring-trace')
      environment['VN_WINEHUA_PRESENT_TRACE'] = '1';
    if (this.perfProfileText === 'direct-fence-wait')
      environment['VN_WINEHUA_DIRECT_FENCE_WAIT'] = '1';
    else if (this.perfProfileText === 'no-remote-sync')
      environment['VN_WINEHUA_REMOTE_MEMORY_SYNC'] = '0';
    else if (this.perfProfileText === 'no-dynamic-flush')
      environment['DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED'] = '0';
    else if (this.perfProfileText === 'fence-feedback')
      environment['VN_PERF'] = '';
    else if (this.perfProfileText === 'shadow-none') {
      environment['VKR_WINEHUA_SHADOW_FROM_HOST'] = 'none';
      environment['VKR_WINEHUA_SHADOW_TRACE'] = '0';
    } else if (this.perfProfileText === 'shadow-trace') {
      environment['VKR_WINEHUA_SHADOW_FROM_HOST'] = 'full';
      environment['VKR_WINEHUA_SHADOW_TRACE'] = '1';
      environment['DXVK_WINEHUA_TRACE_SAMPLED'] = '1';
      environment['DXVK_WINEHUA_TRACE_FLOW'] = '1';
    } else if (this.perfProfileText === 'shadow-to-host-explicit') {
      environment['VKR_WINEHUA_SHADOW_FROM_HOST'] = 'to-host-explicit';
      environment['VKR_WINEHUA_SHADOW_TRACE'] = '0';
    } else if (this.perfProfileText === 'shadow-precise' ||
      this.perfProfileText === 'shadow-precise-single-ring' ||
      this.perfProfileText === 'shadow-precise-sync-submit' ||
      this.perfProfileText === 'shadow-precise-strong-ring' ||
      this.perfProfileText === 'shadow-precise-legacy-host-sync' ||
      this.perfProfileText === 'shadow-precise-dirty-ring' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-no-upload' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-no-upload-fast' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-inline-upload' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-inline-upload-coverage-sort' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-coverage-sort-sampled' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-coverage-poll' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-inline-upload-alias-cover' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-inline-upload-serialized' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-inline-upload-descriptor-serialized' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-frame-assoc-trace' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-present-image-trace' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-perf' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-gpu-frame-profile' ||
      this.perfProfileText === 'shadow-precise-dirty-ring-frame-timeline' ||
      this.perfProfileText === 'shadow-precise-strong-ring-trace' ||
      this.perfProfileText === 'shadow-precise-strong-ring-perf' ||
      this.perfProfileText === 'shadow-precise-strong-ring-async-present' ||
      this.perfProfileText === 'shadow-precise-strong-ring-fence-poll' ||
      this.perfProfileText === 'shadow-precise-strong-ring-mailbox' ||
      this.perfProfileText === 'shadow-precise-direct-fence' ||
      this.perfProfileText === 'shadow-precise-retain-shmem' ||
      this.perfProfileText === 'shadow-precise-cpu-upload') {
      environment['DXVK_WINEHUA_PRECISE_SHADOW'] = '1';
      environment['DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED'] = '1';
      environment['VN_WINEHUA_REMOTE_MEMORY_SYNC'] = '1';
      environment['VKR_WINEHUA_SHADOW_FROM_HOST'] = 'precise';
      environment['VKR_WINEHUA_SHADOW_TRACE'] =
        this.perfProfileText === 'shadow-precise-strong-ring-trace' ? '1' : '0';
      if (this.perfProfileText === 'shadow-precise-single-ring')
        environment['VN_PERF'] = 'no_fence_feedback,no_query_feedback,no_multi_ring';
      if (this.perfProfileText === 'shadow-precise-sync-submit')
        environment['VN_PERF'] =
          'no_fence_feedback,no_query_feedback,no_async_queue_submit';
      if (this.perfProfileText === 'shadow-precise-strong-ring' ||
        this.perfProfileText === 'shadow-precise-legacy-host-sync' ||
        this.perfProfileText === 'shadow-precise-dirty-ring' ||
        this.perfProfileText === 'shadow-precise-dirty-ring-no-upload' ||
        this.perfProfileText === 'shadow-precise-dirty-ring-no-upload-fast' ||
        this.perfProfileText === 'shadow-precise-dirty-ring-inline-upload' ||
        this.perfProfileText === 'shadow-precise-dirty-ring-inline-upload-coverage-sort' ||
        this.perfProfileText === 'shadow-precise-dirty-ring-coverage-sort-sampled' ||
        this.perfProfileText === 'shadow-precise-dirty-ring-coverage-poll' ||
        this.perfProfileText === 'shadow-precise-dirty-ring-inline-upload-alias-cover' ||
        this.perfProfileText === 'shadow-precise-dirty-ring-inline-upload-serialized' ||
        this.perfProfileText === 'shadow-precise-dirty-ring-inline-upload-descriptor-serialized' ||
        this.perfProfileText === 'shadow-precise-dirty-ring-frame-assoc-trace' ||
        this.perfProfileText === 'shadow-precise-dirty-ring-present-image-trace' ||
        this.perfProfileText === 'shadow-precise-dirty-ring-perf' ||
        this.perfProfileText === 'shadow-precise-dirty-ring-gpu-frame-profile' ||
        this.perfProfileText === 'shadow-precise-dirty-ring-frame-timeline' ||
        this.perfProfileText === 'shadow-precise-strong-ring-trace' ||
        this.perfProfileText === 'shadow-precise-strong-ring-perf' ||
        this.perfProfileText === 'shadow-precise-strong-ring-async-present' ||
        this.perfProfileText === 'shadow-precise-strong-ring-fence-poll' ||
        this.perfProfileText === 'shadow-precise-strong-ring-mailbox')
        environment['VN_WINEHUA_STRONG_RING_BARRIER'] = '1';
      if (this.perfProfileText === 'shadow-precise-strong-ring-perf' ||
        this.perfProfileText === 'shadow-precise-dirty-ring-perf')
        environment['VN_WINEHUA_PERF_SUMMARY'] = '1';
      if (this.perfProfileText === 'shadow-precise-direct-fence')
        environment['VN_WINEHUA_DIRECT_FENCE_WAIT'] = '1';
      if (this.perfProfileText === 'shadow-precise-dirty-ring-frame-assoc-trace')
        environment['WINEHUA_DXVK_TRACE_CAMERA'] = '1';
      if (this.perfProfileText === 'shadow-precise-dirty-ring-present-image-trace')
        environment['WINEHUA_DXVK_TRACE_PRESENT_IMAGE'] = '1';
      if (this.perfProfileText === 'shadow-precise-retain-shmem')
        environment['VN_WINEHUA_DEFER_SHMEM_UNREF'] = '1';
      if (this.perfProfileText === 'shadow-precise-cpu-upload')
        environment['VKR_WINEHUA_GPU_UPLOAD'] = '0';
    }
    const overrideCount = Math.min(this.d3dEnvKeys.length, this.d3dEnvValues.length);
    for (let index = 0; index < overrideCount; ++index) {
      environment[this.d3dEnvKeys[index]] = this.d3dEnvValues[index];
    }
    return environment;
  }

  private d3dPresentBackend(): string {
    return this.d3dBackend === 'dxvk_legacy' ? 'venus_broker_present' : 'virgl_compositor';
  }

  private selectD3dBackend = (backend: string): void => {
    const selected = backend === 'dxvk_legacy' ? 'dxvk_legacy' : 'wined3d';
    this.d3dBackend = selected;
    this.d3dBackendText = selected;
    this.d3dPreferenceText = selected;
    this.statusText = selected === 'dxvk_legacy' ? 'DXVK Legacy' : 'WineD3D';
    hilog.info(DOMAIN, 'GameTest', 'D3D backend selected=%{public}s', selected);
  }

  // 引擎操作统一入口: 忙时 (初始化/停止编排中) 由 WineEnvService 自身拦截。
  // 升级检测到不一致时 (upgradePending) 快速操作转为"应用更新" —
  // master 开发版无升级弹卡 UI, doReset 完整恢复出厂 (重装数据+清 C 盘)
  // 即 main-ui 升级卡"立即应用"同语义; 日常路径走 restartEngine 编排
  private quickStart = (): void => {
    if (this.upgradePending) {
      this.env.doReset();
    } else {
      this.env.restartEngine();
    }
  }

  private quickReset = (): void => {
    this.env.doReset();
  }

  private quickRestart = (): void => {
    this.env.restartEngine();
  }

  private doRun = (name: string): void => {
    this.statusText = '启动中...';
    let exePath = this.curDir + '/' + name;
    testNapi.runWineProgram({
      windowsExePath: exePath,
      argv: [],
      environment: this.d3dLaunchEnvironment(),
      workingDirectory: this.curDir,
      prefixMode: 'reuse',
      d3dBackend: this.d3dBackend,
      presentBackend: this.d3dPresentBackend(),
      automationMode: false
    });
  }

  private relPath = (): string => {
    if (this.curDir == DRIVE_C) return 'C:\\';
    return 'C:\\' + this.curDir.substring(DRIVE_C.length + 1).replace(/\//g, '\\');
  }

  private listDir = (path: string): void => {
    try {
      hilog.info(DOMAIN, 'wine', 'listDir: path=%{public}s', path);
      let names = fs.listFileSync(path);
      hilog.info(DOMAIN, 'wine', 'listDir: got %{public}d entries', names.length);
      names.sort((a, b) => {
        let aIsDir = this.isDir(path + '/' + a);
        let bIsDir = this.isDir(path + '/' + b);
        if (aIsDir && !bIsDir) return -1;
        if (!aIsDir && bIsDir) return 1;
        if (!aIsDir && !bIsDir) {
          let aExe = a.toLowerCase().endsWith('.exe') ? 0 : 1;
          let bExe = b.toLowerCase().endsWith('.exe') ? 0 : 1;
          return aExe - bExe || a.localeCompare(b);
        }
        return a.localeCompare(b);
      });
      this.entries = names;
    } catch (e) {
      this.entries = [];
    }
  }

  private isDir = (path: string): boolean => {
    try { return fs.statSync(path).isDirectory(); } catch (_) { return false; }
  }

  private isExe = (name: string): boolean => {
    return name.toLowerCase().endsWith('.exe');
  }

  private enterDir = (name: string): void => {
    let newPath = this.curDir + '/' + name;
    this.dirStack.push(this.curDir);
    this.curDir = newPath;
    this.listDir(newPath);
  }

  private goUp = (): void => {
    let parent = this.dirStack.pop();
    if (parent) {
      this.curDir = parent;
      this.listDir(parent);
    }
  }

  private statusColor(): string {
    if (this.state == 'ready' || this.state == 'ready-degraded') return '#4f4';
    if (this.state == 'failed') return '#f84';
    if (this.state == 'starting' || this.state == 'stopping') return '#8cf';
    return this.prefixReady ? '#8cf' : '#888';
  }

  private engineBusy(): boolean {
    return this.initBusy || this.opBusy;
  }

  build() {
    Stack() {
    Row() {
      // ============================================================
      //  侧边栏
      // ============================================================
      Column() {
        // -- Wine 环境状态 --
        Text('Wine 环境')
          .fontWeight(FontWeight.Bold).fontColor('#999')
          .width('100%').padding({ top: 12, bottom: 6 })

        Row() {
          Circle({ width: 8, height: 8 })
            .fill(this.prefixReady ? '#4f4' : '#f84')
            .margin({ right: 6 })
          Text(this.statusText)
            .fontColor(this.statusColor())
            .layoutWeight(1)
            .maxLines(2).textOverflow({ overflow: TextOverflow.Ellipsis })
        }
        .width('100%').padding({ bottom: 12 })
        .alignItems(VerticalAlign.Center)

        Divider().color('#2a2a2a').width('100%')

        Text('D3D backend')
          .fontWeight(FontWeight.Bold).fontColor('#999')
          .width('100%').padding({ top: 12, bottom: 8 })

        Row({ space: 4 }) {
          Button('WineD3D')
            .layoutWeight(1).height(34)
            .backgroundColor(this.d3dBackend === 'wined3d' ? '#285' : '#333')
            .onClick(() => this.selectD3dBackend('wined3d'))
          Button('DXVK Legacy')
            .layoutWeight(1).height(34)
            .backgroundColor(this.d3dBackend === 'dxvk_legacy' ? '#285' : '#333')
            .onClick(() => this.selectD3dBackend('dxvk_legacy'))
        }
        .width('100%').margin({ bottom: 8 })

        // -- 快速操作 --
        Text('快速操作')
          .fontWeight(FontWeight.Bold).fontColor('#999')
          .width('100%').padding({ top: 12, bottom: 8 })

        if (!this.prefixReady) {
          Button('初始化 Wine')
            .width('100%').backgroundColor('#285')
            .enabled(!this.engineBusy())
            .onClick(this.quickStart)
            .margin({ bottom: 8 })
        } else if (this.upgradePending) {
          // 引擎数据与 HAP 不一致: 引擎未拉起, 唯一动作是应用更新
          // (完整恢复出厂 = main-ui 升级卡"立即应用"同语义)
          Button('应用引擎更新（重置）')
            .width('100%').backgroundColor('#633')
            .enabled(!this.engineBusy())
            .onClick(this.quickReset)
            .margin({ bottom: 8 })
        } else {
          Button('重置 Wine')
            .width('100%').backgroundColor('#633')
            .enabled(!this.engineBusy())
            .onClick(this.quickReset)
            .margin({ bottom: 8 })
          Button('重启 Wine')
            .width('100%').backgroundColor('#652')
            .enabled(!this.engineBusy())
            .onClick(this.quickRestart)
            .margin({ bottom: 8 })
        }

        Button('开发测试（Core）')
          .width('100%').backgroundColor('#246')
          .enabled(!this.engineBusy())
          .onClick(() => this.env.startDeveloperSmoke())
          .margin({ bottom: 8 })

        Divider().color('#2a2a2a').width('100%')

        // -- 运行中进程 --
        Row() {
          Text('运行中进程')
            .fontWeight(FontWeight.Bold).fontColor('#999')
            .layoutWeight(1)
          Button({ type: ButtonType.Circle, stateEffect: true }) {
            Text('↻')
          }
          .width(28).height(28).backgroundColor('#333')
          .onClick(() => ProcessService.getInstance().refresh())
        }
        .width('100%').padding({ top: 12, bottom: 8 })
        .alignItems(VerticalAlign.Center)

        if (this.processList.length > 0) {
          List({ space: 1 }) {
            ForEach(this.processList, (proc: ProcessInfo) => {
              ListItem() {
                Row() {
                  Column() {
                    Text(proc.name)
                      .fontColor('#dfd')
                      .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                  }
                  .layoutWeight(1).alignItems(HorizontalAlign.Start)

                  if (proc.state === 'running') {
                    Circle({ width: 6, height: 6 }).fill('#4f4').margin({ right: 10 })
                  }
                  if (ProcessService.getInstance().isKillable(proc)) {
                    Text('✕')
                      .fontColor('#f55')
                      .onClick(() => ProcessService.getInstance().kill(proc.pid))
                  }
                }
                .width('100%').padding({ left: 14, right: 14, top: 10, bottom: 10 })
                .alignItems(VerticalAlign.Center)
              }
              .backgroundColor('#1e1e1e')
            })
          }
          .width('100%').layoutWeight(1)
          .margin({ left: -14, right: -14 })
          .scrollBar(BarState.Off)
        } else {
          Text(this.prefixReady ? '无运行中进程' : 'Wine 未初始化')
            .fontColor('#555')
            .width('100%').padding({ top: 8 })
        }
      }
      .width(300).height('100%')
      .backgroundColor('#141414')
      .padding({ left: 14, right: 14, top: 10, bottom: 10 })

      // -- 侧边栏分隔线 --
      Divider().vertical(true).color('#2a2a2a').height('100%')

      // ============================================================
      //  右侧主内容区
      // ============================================================
      Column() {
        // -- 标题栏 --
        Row() {
          Text($r('app.string.app_name'))
            .fontWeight(FontWeight.Bold).fontColor('#fff')

          Text(this.statusText)
            .fontColor(this.statusColor())
            .margin({ left: 12 }).layoutWeight(1)
            .maxLines(1)
        }
        .width('100%')
        .padding({ left: 14, right: 14, top: 10, bottom: 10 })
        .backgroundColor('#1a1a1a')
        .alignItems(VerticalAlign.Center)

        // -- 文件浏览器 --
        if (this.prefixReady) {
          Row() {
            if (this.curDir != DRIVE_C) {
              Button('← 上级目录')
                .backgroundColor('#333')
                .onClick(this.goUp)
            }
            Text(this.relPath())
              .fontColor('#888')
              .margin({ left: 10 }).layoutWeight(1)
              .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
          }
          .width('100%').padding({ left: 14, right: 14, top: 10, bottom: 6 })

          List({ space: 1 }) {
            ForEach(this.entries, (name: string) => {
              ListItem() {
                Row() {
                  Text(this.isDir(this.curDir + '/' + name) ? '📁' : (this.isExe(name) ? '▶' : '  '))
                    .width(28)
                  Text(name)
                    .fontColor(this.isExe(name) ? '#dfd' :
                               this.isDir(this.curDir + '/' + name) ? '#8cf' : '#888')
                    .layoutWeight(1)
                    .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                  if (this.isExe(name)) {
                    Button('启动')
                      .height(32)
                      .enabled(!this.engineBusy())
                      .onClick(() => this.doRun(name))
                  }
                }
                .width('100%').height(44).padding({ left: 14, right: 14 })
                .alignItems(VerticalAlign.Center)
                .onClick(() => {
                  if (this.isDir(this.curDir + '/' + name)) {
                    this.enterDir(name);
                  }
                })
              }
              .backgroundColor('#1e1e1e')
            })
          }
          .width('100%').layoutWeight(1)
        } else {
          Column() {
            Text(this.initFailed ? 'Wine 初始化失败，请检查状态后重试或重置' : '需要先初始化 Wine 环境')
              .fontColor('#555')
          }
          .width('100%').layoutWeight(1).justifyContent(FlexAlign.Center)
        }
      }
      .layoutWeight(1).height('100%')
      .backgroundColor('#111')
    }
    .width('100%').height('100%')
    if (this.initOverlayVisible && !this.prefixReady) {
      Column() {
        LoadingProgress()
          .width(42)
          .height(42)
          .color('#7db8ff')
          .margin({ bottom: 18 })
        Text('正在准备 Wine')
          .fontSize(20)
          .fontWeight(FontWeight.Medium)
          .fontColor('#f4f7fb')
          .margin({ bottom: 8 })
        Text(this.initOverlayText)
          .fontSize(15)
          .fontColor('#aeb8c6')
          .maxLines(2)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      .width(360)
      .padding({ left: 28, right: 28, top: 26, bottom: 26 })
      .backgroundColor('#20242b')
      .borderRadius(8)
      .alignItems(HorizontalAlign.Center)
      .shadow({ radius: 18, color: '#66000000', offsetX: 0, offsetY: 8 })
    }
    }
    .width('100%').height('100%')
  }
}
