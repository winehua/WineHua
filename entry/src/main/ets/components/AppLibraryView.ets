import { AppLibraryService, AppEntry, AppConfig, ScanMarks } from '../service/AppLibraryService';
import { WineEnvService } from '../service/WineEnvService';
import { ProcessService, ProcessInfo } from '../service/ProcessService';
import { Breakpoint, BreakpointSystem } from '../common/Breakpoint';
import { EmptyState } from './EmptyState';
import { promptAction, ChipGroup, ChipGroupItemOptions, SegmentButton, SegmentButtonOptions } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0x0000;
const TAG = 'AppLibraryView';

// 分类 chips (设计 4.1): 最近使用/收藏/开始菜单/内建/外部导入;
// 有隐藏条目时追加 hidden ("已隐藏" — 否则"隐藏"菜单项成单向陷阱, 无法找回)。
// 前两者为动态视图 (lastRun 时间戳 / favorite 旗标, 与来源正交),
// 后三者按入库来源互斥归类 (sourceOf: builtin 旗标 > source 字段, 老数据缺省 import)
// 控件: 系统预置 ChipGroup (@kit.ArkUI, API 12+)

// 单应用配置 (菜单"配置"弹半模态): 图形渲染/兼容模式/启动参数三项,
// 全部可缺省 (跟随全局)。刻意不做裸环境变量等无法翻译为用户语言的开关。
// 容器: bindSheet 半模态 (内容会随版本增多, 半模态空间比 CustomDialog 大、层级更轻)
const GRAPHICS_KINDS: Array<string> = ['global', 'dxvk', 'wined3d'];

// chip 定义 (ArkTS 禁止内联对象类型, 需显式 interface)
interface ChipDef {
  kind: string;
  label: string;
}

// 重命名输入对话框 (MenuItem"重命名"弹出)
@CustomDialog
struct RenameDialog {
  controller?: CustomDialogController;
  initial: string = '';
  onDone: (name: string) => void = () => {};
  @State text: string = '';

  aboutToAppear(): void {
    this.text = this.initial;
  }

  build() {
    Column({ space: 14 }) {
      Text('重命名')
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
      TextInput({ text: this.initial })
        .onChange((value: string) => {
          this.text = value;
        })
      Row({ space: 12 }) {
        Button('取消')
          .layoutWeight(1)
          .backgroundColor($r('app.color.control_bg'))
          .onClick(() => this.controller?.close())
        Button('确定')
          .layoutWeight(1)
          .backgroundColor($r('app.color.accent'))
          .onClick(() => {
            this.onDone(this.text);
            this.controller?.close();
          })
      }
      .width('100%')
    }
    .padding(18)
    .backgroundColor($r('app.color.overlay_bg'))
    .borderRadius(12)
  }
}

// 应用库页 (设计 4.1): 搜索框 (右侧"扫描"菜单) + ChipGroup 分类 (最近使用/收藏/
// 开始菜单/内建/外部导入) + 卡片网格 (lg, 列数随宽自适应) / 列表 (sm·md)。
// 条目 (网格卡片与列表行共用 entryItem): 左图标, 右侧两行 — 名称 / exe 文件名 +
// 类型标签 ("内建"·"收藏"小胶囊); 单击启动 (加载态为半透明遮罩+转圈),
// 长按(PC 右键)菜单: 启动/收藏/重命名/配置/隐藏/打开所在目录/从列表移除 (内建工具不可隐藏/移除)。

// entryItem 参数 (ArkTS 多参 Builder 需对象字面量; loadingAsIcon=true 时
// 加载态显示在图标位 — 列表行用; 网格卡片另有整卡遮罩, 传 false)
interface EntryItemSpec {
  entry: AppEntry;
  iconSize: number;
  loadingAsIcon: boolean;
}
@Component
export struct AppLibraryView {
  @State entries: AppEntry[] = [];
  @State prefixReady: boolean = false;
  // 断点镜像: lg → Grid 大卡, sm/md → List
  @State bp: Breakpoint = 'lg';
  @State chip: string = 'recent';
  // 搜索关键字 (名称/路径子串, 大小写不敏感)
  @State searchText: string = '';
  // Grid columnsTemplate: 随可用宽度自适应 (300vp/列, 1~4 列)
  @State gridCols: string = '1fr 1fr';
  // 启动反馈: 点击后卡片进入加载态。成功 = 受理 pid 3s 后仍在进程表
  // (getProcessList 只含 running, 闪退进程届时已被 ProcMon 摘除);
  // 失败 = 受理即失败 / evt:launch-failed / 该 pid 的 evt:proc-exited。
  // 不再用 wine-running 当成功信号 — 它的语义只是"broker 受理 spawn",
  // 闪退程序会被误判成功
  @State launchingPath: string = '';
  // 扫描进行中状态由 AppLibraryService 持有 (scanning/scanLabel), 全窗遮罩
  // 渲染在 Index 根 Stack — 要盖住侧栏, 页面内遮罩只覆盖内容区 (bindContentCover
  // 方案实测不弹出, 且拦截范围不够); 本页只经 runScan 触发扫描
  // -- 单应用配置半模态状态 (openConfig 填初值, saveConfig 归一缺省后落库) --
  @State showConfig: boolean = false;
  @State cfgGraphicsSel: number[] = [0];
  @State cfgCompat: boolean = false;
  @State cfgArgs: string = '';
  private cfgEntryPath: string = '';
  private cfgEntryName: string = '';
  // 空态"去文件页"回调, 由 Index 注入 (切换 Tabs 到文件页), 有才显示按钮
  onNavigateToFiles?: () => void;
  // "打开所在目录"回调, 由 Index 注入 (定位文件页到目录), 有才显示该菜单项
  onOpenDir?: (dirPath: string) => void;

  private lib: AppLibraryService = AppLibraryService.getInstance();
  private env: WineEnvService = WineEnvService.getInstance();
  private launchTimer: number = -1;
  private launchingPid: number = -1;
  private renameController: CustomDialogController | null = null;

  private mirror = (): void => {
    const wasReady = this.prefixReady;
    this.entries = this.lib.entries;
    this.prefixReady = this.env.prefixReady;
    // prefix 就绪 false→true 边沿: 补内建工具入库 (service 内部幂等,
    // 边沿触发避免 mirror↔notify 反馈回路)
    if (this.prefixReady && !wasReady) {
      this.lib.ensureBuiltinTools();
    }
  }

  // 启动反馈事件流: 只关心本次受理 pid 的退出与全局 launch-failed
  private onLaunchEvent = (evt: string): void => {
    if (!this.launchingPath) {
      return;
    }
    if (evt == 'launch-failed') {
      this.failLaunching('程序启动失败。可尝试切换渲染策略（环境页）后重试');
      return;
    }
    if (evt.startsWith('proc-exited:')) {
      const pid = Number(evt.substring(12));
      if (pid == this.launchingPid) {
        this.failLaunching('程序启动后闪退。可尝试切换渲染策略（环境页）或开启兼容模式后重试');
      }
    }
  }

  aboutToAppear(): void {
    this.lib.subscribe(this.mirror);
    this.env.subscribe(this.mirror);
    this.env.subscribeEvents(this.onLaunchEvent);
    const bps = BreakpointSystem.getInstance();
    bps.subscribe(() => {
      this.bp = bps.current;
    });
    this.bp = bps.current;
    this.mirror();
  }

  private clearLaunching(): void {
    this.launchingPath = '';
    this.launchingPid = -1;
    if (this.launchTimer >= 0) {
      clearTimeout(this.launchTimer);
      this.launchTimer = -1;
    }
  }

  // 启动失败: 读状态清理 + 失败对话框 (可读原因 + 导出日志入口, 设计 4.1)
  private failLaunching(reason: string): void {
    this.clearLaunching();
    AlertDialog.show({
      title: '启动失败',
      message: reason,
      primaryButton: {
        value: '知道了',
        action: () => {}
      },
      secondaryButton: {
        value: '导出日志',
        action: () => {
          const path = this.env.exportLogs();
          promptAction.showToast({ message: path ? '日志已导出到下载目录' : '日志导出失败' });
        }
      }
    });
  }

  private async launch(entry: AppEntry): Promise<void> {
    // 判据 = 状态机 ready (涵盖 prefix 就绪/wineserver 健康), 不再各自比对状态串;
    // launchingPath 兼作防重入 — ensureDesktop 等待窗口期重复点击不会启动多个实例
    if (!this.env.canLaunch() || this.launchingPath) {
      return;
    }
    // 已就绪但无桌面会话：先拉起桌面，避免程序启动后只有程序窗口没有桌面
    if (!this.env.sessionAlive) {
      this.launchingPath = entry.path;   // 占位: 卡片进入加载态 + 防重入
      this.launchingPid = -1;
      const ok = await this.env.ensureDesktop();
      if (!ok) {
        this.failLaunching('桌面启动失败。可尝试到环境页重启引擎后重试');
        return;   // failLaunching 内部 clearLaunching 清占位
      }
    }
    this.clearLaunching();
    const pid = this.lib.launch(entry.path);
    if (pid <= 0) {
      this.failLaunching('程序启动失败。可尝试切换渲染策略（环境页）后重试');
      return;
    }
    this.launchingPath = entry.path;
    this.launchingPid = pid;
    // 3s 存活判成功: 闪退进程此时已被 ProcMon (1Hz) 摘除; proc-exited 事件
    // 先到则立即判失败 (见 onLaunchEvent)
    this.launchTimer = setTimeout(() => {
      if (!this.launchingPath) {
        return;
      }
      const alive = ProcessService.getInstance().processList
        .some((p: ProcessInfo) => p.pid == this.launchingPid);
      if (alive) {
        this.clearLaunching();
      } else {
        this.failLaunching('程序启动后闪退。可尝试切换渲染策略（环境页）或开启兼容模式后重试');
      }
    }, 3000);
  }

  private confirmRemove(entry: AppEntry): void {
    AlertDialog.show({
      title: '移除应用',
      message: `确定从应用库移除「${entry.name}」吗？（不会删除文件）`,
      primaryButton: {
        value: '取消',
        action: () => {}
      },
      secondaryButton: {
        value: '移除',
        fontColor: $r('app.color.danger'),
        action: () => {
          this.lib.remove(entry.path);
        }
      }
    });
  }

  private openRename(entry: AppEntry): void {
    this.renameController = new CustomDialogController({
      builder: RenameDialog({
        initial: entry.name,
        onDone: (name: string) => {
          this.lib.rename(entry.path, name);
        }
      }),
      autoCancel: true
    });
    this.renameController.open();
  }

  // 单应用配置: 读取现值作初值打开半模态; 保存时把 'global'/false/空串 归一为缺省
  // (字段不写 = 跟随全局, 持久化 JSON 保持干净)
  private openConfig(entry: AppEntry): void {
    const cfg = entry.config;
    this.cfgEntryPath = entry.path;
    this.cfgEntryName = entry.name;
    const g = cfg?.graphics ?? 'global';
    const gi = GRAPHICS_KINDS.indexOf(g);
    this.cfgGraphicsSel = [gi >= 0 ? gi : 0];
    this.cfgCompat = cfg?.compat ?? false;
    this.cfgArgs = cfg?.args ?? '';
    this.showConfig = true;
  }

  private saveConfig(): void {
    const config: AppConfig = {};
    const kind = GRAPHICS_KINDS[this.cfgGraphicsSel[0] ?? 0];
    if (kind != 'global') {
      config.graphics = kind as 'dxvk' | 'wined3d';
    }
    if (this.cfgCompat) {
      config.compat = true;
    }
    const trimmed = this.cfgArgs.trim();
    if (trimmed) {
      config.args = trimmed;
    }
    this.lib.setConfig(this.cfgEntryPath, config);
    this.showConfig = false;
  }

  // exe 路径 → 所在目录 (去掉最后一段文件名)
  private dirOf(path: string): string {
    const idx = path.lastIndexOf('/');
    return idx > 0 ? path.substring(0, idx) : path;
  }

  // exe 文件名 (路径最后一段) — 条目副标题
  private baseName(path: string): string {
    const idx = path.lastIndexOf('/');
    return idx >= 0 ? path.substring(idx + 1) : path;
  }

  // 当前 chip + 搜索关键字过滤后的条目。
  // 有搜索词时跨分类全局搜 (五分类互斥后, 片内搜索会让"明明在库的应用"
  // 在别的分类下搜不到, 比不过滤更迷惑); 隐藏条目不参与全局搜索
  private filtered(): AppEntry[] {
    const kw = this.searchText.toLowerCase();
    if (kw) {
      return this.entries.filter((e: AppEntry) => !e.hidden &&
        (e.name.toLowerCase().includes(kw) || e.path.toLowerCase().includes(kw)));
    }
    switch (this.chip) {
      case 'recent':
        // filter 返回新数组, 原地 sort 安全; lastRun 新→旧
        return this.entries.filter((e: AppEntry) => e.lastRun !== undefined && !e.hidden)
          .sort((a: AppEntry, b: AppEntry) => (b.lastRun ?? 0) - (a.lastRun ?? 0));
      case 'favorite':
        return this.entries.filter((e: AppEntry) => e.favorite && !e.hidden);
      case 'startmenu':
        return this.entries.filter((e: AppEntry) => this.lib.sourceOf(e) == 'lnk' && !e.hidden);
      case 'builtin':
        return this.entries.filter((e: AppEntry) => e.builtin && !e.hidden);
      case 'import':
        return this.entries.filter((e: AppEntry) => this.lib.sourceOf(e) == 'import' && !e.hidden);
      case 'hidden':
        return this.entries.filter((e: AppEntry) => e.hidden);
      default:
        return this.entries.filter((e: AppEntry) => !e.hidden);
    }
  }

  private hasHidden(): boolean {
    return this.entries.some((e: AppEntry) => e.hidden);
  }

  // chip 定义 (kind + 文案): hidden 项仅在有隐藏条目或正处于 hidden 时出现
  private chipDefs(): ChipDef[] {
    const defs: ChipDef[] = [
      { kind: 'recent', label: '最近使用' },
      { kind: 'favorite', label: '收藏' },
      { kind: 'startmenu', label: '开始菜单' },
      { kind: 'builtin', label: '内建' },
      { kind: 'import', label: '外部导入' }
    ];
    if (this.hasHidden() || this.chip == 'hidden') {
      defs.push({ kind: 'hidden', label: '已隐藏' });
    }
    return defs;
  }

  // 各分类空态文案 (有搜索词时统一为"没有匹配")
  private emptyTitle(): string {
    if (this.searchText) {
      return '没有匹配的应用';
    }
    switch (this.chip) {
      case 'recent': return '还没有运行记录';
      case 'favorite': return '还没有收藏';
      case 'startmenu': return '没有开始菜单快捷方式';
      case 'builtin': return '内建工具未就绪';
      case 'import': return '还没有导入的应用';
      case 'hidden': return '没有隐藏的应用';
      default: return '应用库为空';
    }
  }

  private emptyDesc(): string {
    if (this.searchText) {
      return '';
    }
    switch (this.chip) {
      case 'recent': return '从应用库或文件页启动应用后，会显示在这里';
      case 'favorite': return '长按应用卡片，选择「收藏」';
      case 'startmenu': return '安装 Windows 程序后，扫描它的开始菜单快捷方式';
      case 'builtin': return '环境初始化完成后自动加入';
      case 'import': return '把 Windows 程序放入下载目录 (Z:)，或在文件页导入';
      default: return '';
    }
  }

  // 扫描菜单 (搜索框右侧"扫描"点击弹出): 两个入库来源分别对应一个分类 ——
  // 开始菜单 = lnk 解析 (scanShortcuts), 外部导入 = Z: 目录 exe (scanZExes)
  @Builder
  scanMenu() {
    Menu() {
      MenuItem({ content: '扫描开始菜单' })
        .onChange(() => {
          this.runScan('lnk');
        })
      MenuItem({ content: '扫描 Z: 目录' })
        .onChange(() => {
          this.runScan('z');
        })
    }
  }

  // 扫描统一入口 (菜单与空态按钮共用)。扫描期间 Index 根 Stack 的全窗遮罩
  // 拦触摸 (状态由 service 持有), 防扫描中误操作造成状态交错; lib.scanning
  // 兼作防重入。扫描内部是异步 fs (LnkScanner/collectExe 均为 Promise 版),
  // 事件循环保持活性 — 遮罩转圈动画能正常播放; 若用同步 API, 遮罩自己都会冻结
  private async runScan(kind: string): Promise<void> {
    if (this.lib.scanning) {
      return;
    }
    const t0 = Date.now();
    hilog.info(DOMAIN, TAG, 'runScan start kind=%{public}s', kind);
    try {
      if (kind == 'lnk') {
        const r: ScanMarks = await this.lib.scanShortcuts();
        promptAction.showToast({
          message: r.added > 0 ? `新增 ${r.added} 个应用`
            : r.classified > 0 ? `${r.classified} 个应用归入开始菜单`
            : '没有发现新快捷方式'
        });
      } else {
        const added = await this.lib.scanZExes();
        promptAction.showToast({ message: added > 0 ? `新增 ${added} 个应用` : '没有发现新程序' });
      }
    } finally {
      hilog.info(DOMAIN, TAG, 'runScan end kind=%{public}s elapsed=%{public}dms', kind, Date.now() - t0);
    }
  }

  // 条目菜单 (右键与长按通用, 设计 4.1)
  @Builder
  entryMenu(entry: AppEntry) {
    Menu() {
      MenuItem({ content: '启动' })
        .onChange(() => {
          this.launch(entry);
        })
      MenuItem({ content: entry.favorite ? '取消收藏' : '收藏' })
        .onChange(() => {
          this.lib.toggleFavorite(entry.path);
        })
      MenuItem({ content: '重命名' })
        .onChange(() => {
          this.openRename(entry);
        })
      MenuItem({ content: '配置' })
        .onChange(() => {
          this.openConfig(entry);
        })
      if (!entry.builtin) {
        MenuItem({ content: entry.hidden ? '取消隐藏' : '隐藏' })
          .onChange(() => {
            this.lib.setHidden(entry.path, !entry.hidden);
          })
      }
      if (this.onOpenDir) {
        MenuItem({ content: '打开所在目录' })
          .onChange(() => {
            if (this.onOpenDir) {
              this.onOpenDir(this.dirOf(entry.path));
            }
          })
      }
      if (!entry.builtin) {
        MenuItem({ content: '从列表移除' })
          .contentFontColor($r('app.color.danger'))
          .onChange(() => {
            this.confirmRemove(entry);
          })
      }
    }
  }

  // 单应用配置半模态内容 (bindSheet builder)
  @Builder
  configSheet() {
    Column({ space: 16 }) {
      Text(`配置 · ${this.cfgEntryName}`)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))

      Column({ space: 8 }) {
        Text('图形渲染')
          .fontSize(13)
          .fontColor($r('app.color.text_secondary'))
        SegmentButton({
          options: SegmentButtonOptions.capsule({
            buttons: [{ text: '跟随全局' }, { text: '硬件加速' }, { text: '兼容渲染' }],
            multiply: false
          }),
          selectedIndexes: $cfgGraphicsSel,
          onItemClicked: (index: number) => {
            this.cfgGraphicsSel = [index];
          }
        })
      }
      .width('100%')
      .alignItems(HorizontalAlign.Start)

      Row() {
        Column({ space: 2 }) {
          Text('兼容模式')
            .fontSize(13)
            .fontColor($r('app.color.text_secondary'))
          Text('应用闪退或画面异常时开启，运行速度会变慢')
            .fontSize(11)
            .fontColor($r('app.color.text_tertiary'))
        }
        .layoutWeight(1)
        .alignItems(HorizontalAlign.Start)
        Toggle({ type: ToggleType.Switch, isOn: this.cfgCompat })
          .onChange((isOn: boolean) => {
            this.cfgCompat = isOn;
          })
      }
      .width('100%')

      Column({ space: 8 }) {
        Text('启动参数')
          .fontSize(13)
          .fontColor($r('app.color.text_secondary'))
        TextInput({ placeholder: '如 -windowed，留空为无', text: this.cfgArgs })
          .onChange((value: string) => {
            this.cfgArgs = value;
          })
      }
      .width('100%')
      .alignItems(HorizontalAlign.Start)

      Row({ space: 12 }) {
        // 恢复默认只重置本表单的选项, 仍需点保存生效 (防误触)
        Button('恢复默认')
          .layoutWeight(1)
          .backgroundColor($r('app.color.control_bg'))
          .onClick(() => {
            this.cfgGraphicsSel = [0];
            this.cfgCompat = false;
            this.cfgArgs = '';
          })
        Button('保存')
          .layoutWeight(1)
          .backgroundColor($r('app.color.accent'))
          .onClick(() => {
            this.saveConfig();
          })
      }
      .width('100%')
    }
    .padding({ left: 18, right: 18, top: 8, bottom: 24 })
  }

  // 条目内容 (Grid 卡片与 sm/md 列表行共用, 参考 .temp/ui 设计稿):
  // 左图标, 右侧两行 — 上行名称, 下行 exe 文件名 + 类型标签 ("内建"·"收藏" 小胶囊)
  @Builder
  entryItem($$: EntryItemSpec) {
    Row({ space: 10 }) {
      if ($$.loadingAsIcon && this.launchingPath == $$.entry.path) {
        LoadingProgress()
          .width(22).height(22)
          .color($r('app.color.accent'))
      } else if ($$.entry.icon) {
        Image(`file://${$$.entry.icon}`)
          .width($$.iconSize).height($$.iconSize)
      } else {
        // 无图标占位: 系统播放符号 (历史沿用 ▶ 语义)
        SymbolGlyph($r('sys.symbol.play_fill'))
          .fontSize($$.iconSize - 8)
          .fontColor([$r('app.color.accent')])
          .width($$.iconSize).height($$.iconSize)
      }
      Column({ space: 3 }) {
        Text($$.entry.name)
          .fontSize(14)
          .fontColor($r('app.color.text_primary'))
          .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
        Row({ space: 6 }) {
          Text(this.baseName($$.entry.path))
            .fontSize(11)
            .fontColor($r('app.color.text_tertiary'))
            .layoutWeight(1)
            .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
          if ($$.entry.builtin) {
            Text('内建')
              .fontSize(9)
              .fontColor($r('app.color.text_secondary'))
              .padding({ left: 5, right: 5, top: 1, bottom: 1 })
              .backgroundColor($r('app.color.control_bg'))
              .borderRadius(4)
          }
          if ($$.entry.favorite) {
            Text('收藏')
              .fontSize(9)
              .fontColor($r('app.color.accent'))
              .padding({ left: 5, right: 5, top: 1, bottom: 1 })
              .backgroundColor($r('app.color.accent_bg'))
              .borderRadius(4)
          }
        }
        .width('100%')
      }
      .layoutWeight(1)
      .alignItems(HorizontalAlign.Start)
    }
    .width('100%')
    .alignItems(VerticalAlign.Center)
  }

  // lg 网格卡片: entryItem + 启动反馈整卡遮罩 (图标保持可见)
  @Builder
  gridCard(entry: AppEntry) {
    Stack() {
      this.entryItem({ entry: entry, iconSize: 44, loadingAsIcon: false })
      if (this.launchingPath == entry.path) {
        Column() {
          LoadingProgress()
            .width(32).height(32)
            .color($r('app.color.accent'))
        }
        .width('100%').height('100%')
        .justifyContent(FlexAlign.Center)
        .backgroundColor('#99000000')
        .borderRadius(12)
      }
    }
    .width('100%').height(76)
    .padding({ left: 12, right: 12 })
    .backgroundColor($r('app.color.card_bg'))
    .borderRadius(12)
    // 手势挂在卡片实体节点上 (与重构前一致): GridItem 是虚拟容器,
    // 长按手势派发与实体节点不同, 挂 GridItem 上实测长按菜单不弹出
    .onClick(() => this.launch(entry))
    .hoverEffect(HoverEffect.Highlight)
    .bindContextMenu(this.entryMenu(entry), ResponseType.LongPress)
  }

  build() {
    Column() {
      // 首行: 搜索框 + 扫描入口 (扫描固定最右; 菜单: 扫描开始菜单 / 扫描 Z: 目录)。
      // 页头不放"应用库"大标题 — 侧栏已标识当前页, 标题只占纵向空间
      Row() {
        Search({ placeholder: '搜索应用名称或路径' })
          .height(36)
          .layoutWeight(1)
          .onChange((value: string) => {
            this.searchText = value.trim();
          })
        Text('扫描')
          .fontColor($r('app.color.accent'))
          .padding({ left: 12, top: 4, bottom: 4 })
          .bindMenu(this.scanMenu())
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)
      .padding({ left: 14, right: 14, top: 8 })

      // 筛选 chips: 系统预置 ChipGroup, 单选
      ChipGroup({
        items: this.chipDefs().map((d): ChipGroupItemOptions => ({ label: { text: d.label } })),
        selectedIndexes: [Math.max(0, this.chipDefs().findIndex((d) => d.kind == this.chip))],
        multiple: false,
        onChange: (sel: Array<number>) => {
          // 单选再点已选项时回调空数组, 忽略以保留当前选中
          if (sel.length > 0) {
            this.chip = this.chipDefs()[sel[0]].kind;
          }
        }
      })
      .padding({ left: 14, right: 14, top: 6, bottom: 8 })

      Stack() {
      if (this.filtered().length == 0) {
        Column() {
          EmptyState({
            icon: $r('sys.symbol.square_grid_2x2'),
            title: this.emptyTitle(),
            desc: this.emptyDesc()
          })
          // 可扫描的分类在空态给出直达按钮 (搜索框旁"扫描"菜单的替身, 减少发现成本)
          if (!this.searchText && (this.chip == 'startmenu' || this.chip == 'import')) {
            Row({ space: 12 }) {
              if (this.chip == 'startmenu') {
                Button('扫描开始菜单')
                  .backgroundColor($r('app.color.control_bg'))
                  .onClick(() => {
                    this.runScan('lnk');
                  })
              } else {
                Button('扫描 Z: 目录')
                  .backgroundColor($r('app.color.control_bg'))
                  .onClick(() => {
                    this.runScan('z');
                  })
                if (this.onNavigateToFiles) {
                  Button('去文件页')
                    .backgroundColor($r('app.color.control_bg'))
                    .onClick(() => {
                      if (this.onNavigateToFiles) {
                        this.onNavigateToFiles();
                      }
                    })
                }
              }
            }
            .margin({ top: 8 })
          }
        }
        .width('100%').height('100%')
        .justifyContent(FlexAlign.Center)
        .backgroundColor($r('app.color.window_bg'))
      } else if (this.bp == 'lg') {
        // lg 卡片栅格 (Pad 全屏 / PC 宽窗): 横向双行卡片较宽, 按 300vp/列取 1~4 列
        Grid() {
          ForEach(this.filtered(), (entry: AppEntry) => {
            GridItem() {
              this.gridCard(entry)
            }
          }, (entry: AppEntry) => entry.path)
        }
        .columnsTemplate(this.gridCols)
        .columnsGap(12)
        .rowsGap(12)
        .padding(12)
        .width('100%').height('100%')
        .backgroundColor($r('app.color.window_bg'))
        .onAreaChange((_oldValue: Area, newValue: Area) => {
          // 断点 lg 一刀切会窗口稍窄挤、超宽稀; 按可用宽度算列数
          const cols = Math.max(1, Math.min(4, Math.floor((newValue.width as number) / 300)));
          const tpl = '1fr '.repeat(cols).trim();
          if (tpl != this.gridCols) {
            this.gridCols = tpl;
          }
        })
      } else {
        // sm/md 列表 (与网格共用 entryItem, 加载态在图标位)
        List({ space: 1 }) {
          ForEach(this.filtered(), (entry: AppEntry) => {
            ListItem() {
              // Builder 调用返回 void 不能链式挂属性, 包一层 Row (实体节点) 承载手势
              Row() {
                this.entryItem({ entry: entry, iconSize: 36, loadingAsIcon: true })
              }
              .width('100%').height(60)
              .padding({ left: 14, right: 14 })
              // 手势挂在行实体节点上 (与重构前一致, 见 Grid 卡片同注)
              .onClick(() => this.launch(entry))
              .hoverEffect(HoverEffect.Highlight)
              .bindContextMenu(this.entryMenu(entry), ResponseType.LongPress)
            }
            .backgroundColor($r('app.color.card_bg'))
          }, (entry: AppEntry) => entry.path)
        }
        .width('100%').height('100%')
        .scrollBar(BarState.Off)
        .backgroundColor($r('app.color.window_bg'))
      }
      }
      .layoutWeight(1).width('100%')
    }
    .width('100%').height('100%')
    // 根容器统一背景: 启动器下层是桌面 XComponent, 根透明会透出桌面 (顶行/分支不再单设)
    .backgroundColor($r('app.color.window_bg'))
    // 单应用配置半模态 (替代原 CustomDialog: 配置项会增多, 半模态空间更大层级更轻)
    .bindSheet($$this.showConfig, this.configSheet(), {
      height: SheetSize.FIT_CONTENT,
      dragBar: true,
      showClose: true,
      backgroundColor: $r('app.color.overlay_bg')
    })
  }
}
