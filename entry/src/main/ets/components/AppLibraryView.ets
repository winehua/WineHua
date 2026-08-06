import { AppLibraryService, AppEntry, AppConfig } from '../service/AppLibraryService';
import { WineEnvService } from '../service/WineEnvService';
import { Breakpoint, BreakpointSystem } from '../common/Breakpoint';
import { promptAction } from '@kit.ArkUI';

// 筛选 chips (设计 4.1): all/favorite/recent/builtin;
// 有隐藏条目时追加 hidden ("已隐藏" — 否则"隐藏"菜单项成单向陷阱, 无法找回)

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

// 单应用配置对话框 (MenuItem"配置"弹出): 图形渲染/兼容模式/启动参数三项,
// 全部可缺省 (跟随全局)。刻意不做裸环境变量等无法翻译为用户语言的开关
@CustomDialog
struct ConfigDialog {
  controller?: CustomDialogController;
  entryName: string = '';
  initialGraphics: string = 'global'; // 'global' | 'dxvk' | 'wined3d'
  initialCompat: boolean = false;
  initialArgs: string = '';
  onDone: (graphics: string, compat: boolean, args: string) => void = () => {};
  @State graphics: string = 'global';
  @State compat: boolean = false;
  @State args: string = '';

  aboutToAppear(): void {
    this.graphics = this.initialGraphics;
    this.compat = this.initialCompat;
    this.args = this.initialArgs;
  }

  @Builder
  graphicsOption(label: string, value: string) {
    Text(label)
      .fontSize(13)
      .fontColor(this.graphics == value ? $r('app.color.header_bg') : $r('app.color.text_secondary'))
      .padding({ left: 12, right: 12, top: 6, bottom: 6 })
      .backgroundColor(this.graphics == value ? $r('app.color.accent') : $r('app.color.control_bg'))
      .borderRadius(8)
      .onClick(() => {
        this.graphics = value;
      })
  }

  build() {
    Column({ space: 16 }) {
      Text(`配置 · ${this.entryName}`)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))

      Column({ space: 8 }) {
        Text('图形渲染')
          .fontSize(13)
          .fontColor($r('app.color.text_secondary'))
        Row({ space: 8 }) {
          this.graphicsOption('跟随全局', 'global')
          this.graphicsOption('硬件加速', 'dxvk')
          this.graphicsOption('兼容渲染', 'wined3d')
        }
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
        Toggle({ type: ToggleType.Switch, isOn: this.compat })
          .onChange((isOn: boolean) => {
            this.compat = isOn;
          })
      }
      .width('100%')

      Column({ space: 8 }) {
        Text('启动参数')
          .fontSize(13)
          .fontColor($r('app.color.text_secondary'))
        TextInput({ placeholder: '如 -windowed，留空为无', text: this.initialArgs })
          .onChange((value: string) => {
            this.args = value;
          })
      }
      .width('100%')
      .alignItems(HorizontalAlign.Start)

      Row({ space: 12 }) {
        // 恢复默认只重置本对话框的选项, 仍需点保存生效 (防误触)
        Button('恢复默认')
          .layoutWeight(1)
          .backgroundColor($r('app.color.control_bg'))
          .onClick(() => {
            this.graphics = 'global';
            this.compat = false;
            this.args = '';
          })
        Button('取消')
          .layoutWeight(1)
          .backgroundColor($r('app.color.control_bg'))
          .onClick(() => this.controller?.close())
        Button('保存')
          .layoutWeight(1)
          .backgroundColor($r('app.color.accent'))
          .onClick(() => {
            this.onDone(this.graphics, this.compat, this.args);
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

// 应用库页 (设计 4.1): 标题 + 扫描入口 + 筛选 chips + 卡片网格 (lg) / 列表 (sm·md)。
// 卡片: 图标/名称/路径副标题/内建角标; 单击启动 (带启动反馈), 长按(PC 右键)菜单:
// 启动/收藏/重命名/配置/隐藏/打开所在目录/从列表移除 (内建工具不可隐藏/移除)。
@Component
export struct AppLibraryView {
  @State entries: AppEntry[] = [];
  // env.state 镜像: 启动守卫 (wine 初始化中禁止启动) + 启动反馈状态机输入
  @State envState: string = 'idle';
  @State prefixReady: boolean = false;
  // 断点镜像: lg → Grid 大卡, sm/md → List
  @State bp: Breakpoint = 'lg';
  @State chip: string = 'all';
  // 启动反馈: 点击后卡片进入加载态, wine-running 即成功; wine-failed 或超时给失败对话框
  @State launchingPath: string = '';
  // 空态"去文件页"回调, 由 Index 注入 (切换 Tabs 到文件页), 有才显示按钮
  onNavigateToFiles?: () => void;
  // "打开所在目录"回调, 由 Index 注入 (定位文件页到目录), 有才显示该菜单项
  onOpenDir?: (dirPath: string) => void;

  private lib: AppLibraryService = AppLibraryService.getInstance();
  private env: WineEnvService = WineEnvService.getInstance();
  private launchTimer: number = -1;
  private renameController: CustomDialogController | null = null;
  private configController: CustomDialogController | null = null;
  // 上一次镜像到的原始 state 串 (启动反馈状态机的迁移检测, 见 mirror)
  private prevRawMsg: string = '';

  private mirror = (): void => {
    const wasReady = this.prefixReady;
    this.entries = this.lib.entries;
    this.envState = this.env.state;
    this.prefixReady = this.env.prefixReady;
    // prefix 就绪 false→true 边沿: 补内建工具入库 (service 内部幂等,
    // 边沿触发避免 mirror↔notify 反馈回路)
    if (this.prefixReady && !wasReady) {
      this.lib.ensureBuiltinTools();
    }
    // 启动反馈状态机: wine-running = 窗口已弹 (成功信号); wine-failed = 失败。
    // 判定依据原始 state 串变化 (每次启动 native 发新的 "pid:wine-running") —
    // 不能比较干净状态名: 已有程序在跑时 state 本来就是 'wine-running',
    // 再启动第二个不发生状态名迁移, 会漏判成功
    const raw = this.env.lastStateMsg;
    if (this.launchingPath && raw != this.prevRawMsg) {
      if (this.envState == 'wine-running') {
        this.clearLaunching();
      } else if (this.envState == 'wine-failed') {
        this.failLaunching('程序启动失败。可尝试切换渲染策略（环境页）后重试');
      }
    }
    this.prevRawMsg = raw;
  }

  aboutToAppear(): void {
    this.lib.subscribe(this.mirror);
    this.env.subscribe(this.mirror);
    const bps = BreakpointSystem.getInstance();
    bps.subscribe(() => {
      this.bp = bps.current;
    });
    this.bp = bps.current;
    this.mirror();
  }

  private clearLaunching(): void {
    this.launchingPath = '';
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

  private launch(entry: AppEntry): void {
    if (this.envState == 'wineserver-starting' || this.envState == 'wineboot-starting') {
      return;
    }
    this.clearLaunching();
    this.launchingPath = entry.path;
    // 超时兜底: 15s 内既无 running 也无 failed, 提示后清除加载态
    this.launchTimer = setTimeout(() => {
      if (this.launchingPath == entry.path) {
        this.failLaunching('未检测到程序窗口弹出。程序可能已闪退，或与当前渲染策略不兼容');
      }
    }, 15000);
    this.lib.launch(entry.path);
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

  // 单应用配置: 读取现值作初值; 保存时把 'global'/false/空串 归一为缺省
  // (字段不写 = 跟随全局, 持久化 JSON 保持干净)
  private openConfig(entry: AppEntry): void {
    const cfg = entry.config;
    this.configController = new CustomDialogController({
      builder: ConfigDialog({
        entryName: entry.name,
        initialGraphics: cfg?.graphics ?? 'global',
        initialCompat: cfg?.compat ?? false,
        initialArgs: cfg?.args ?? '',
        onDone: (graphics: string, compat: boolean, args: string) => {
          const config: AppConfig = {};
          if (graphics != 'global') {
            config.graphics = graphics;
          }
          if (compat) {
            config.compat = true;
          }
          const trimmed = args.trim();
          if (trimmed) {
            config.args = trimmed;
          }
          this.lib.setConfig(entry.path, config);
        }
      }),
      autoCancel: true
    });
    this.configController.open();
  }

  // exe 路径 → 所在目录 (去掉最后一段文件名)
  private dirOf(path: string): string {
    const idx = path.lastIndexOf('/');
    return idx > 0 ? path.substring(0, idx) : path;
  }

  // 当前 chip 过滤后的条目
  private filtered(): AppEntry[] {
    switch (this.chip) {
      case 'favorite':
        return this.entries.filter((e: AppEntry) => e.favorite && !e.hidden);
      case 'recent':
        return this.entries.filter((e: AppEntry) => (e.lastRun ?? 0) > 0 && !e.hidden)
          .sort((a: AppEntry, b: AppEntry) => (b.lastRun ?? 0) - (a.lastRun ?? 0));
      case 'builtin':
        return this.entries.filter((e: AppEntry) => e.builtin && !e.hidden);
      case 'hidden':
        return this.entries.filter((e: AppEntry) => e.hidden);
      default:
        return this.entries.filter((e: AppEntry) => !e.hidden);
    }
  }

  private hasHidden(): boolean {
    return this.entries.some((e: AppEntry) => e.hidden);
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

  @Builder
  chipItem(label: string, kind: string) {
    Text(label)
      .fontSize(13)
      .fontColor(this.chip == kind ? $r('app.color.header_bg') : $r('app.color.text_secondary'))
      .padding({ left: 12, right: 12, top: 5, bottom: 5 })
      .backgroundColor(this.chip == kind ? $r('app.color.accent') : $r('app.color.control_bg'))
      .borderRadius(12)
      .onClick(() => {
        this.chip = kind;
      })
  }

  build() {
    Column() {
      // 顶行: 标题 + 扫描快捷方式入口 (手动触发, 结果 toast)
      Row() {
        Text('应用库')
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary'))
          .layoutWeight(1)
        Text('扫描快捷方式')
          .fontColor($r('app.color.accent'))
          .padding({ left: 12, top: 4, bottom: 4 })
          .onClick(() => {
            const added = this.lib.scanShortcuts();
            promptAction.showToast({ message: added > 0 ? `新增 ${added} 个应用` : '没有发现新快捷方式' });
          })
      }
      .width('100%')
      .padding({ left: 14, right: 14, top: 8, bottom: 4 })

      // 筛选 chips
      Row({ space: 8 }) {
        this.chipItem('全部', 'all')
        this.chipItem('收藏', 'favorite')
        this.chipItem('最近运行', 'recent')
        this.chipItem('内建工具', 'builtin')
        if (this.hasHidden() || this.chip == 'hidden') {
          this.chipItem('已隐藏', 'hidden')
        }
      }
      .width('100%')
      .padding({ left: 14, right: 14, top: 4, bottom: 8 })

      Stack() {
      if (this.filtered().length == 0) {
        Column() {
          Text(this.chip == 'all' ? '应用库为空' : '该分类下没有应用')
            .fontSize(20)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .margin({ bottom: 8 })
          if (this.chip == 'all') {
            Text('把 Windows 程序放入下载目录，或在文件页导入，然后点重新扫描')
              .fontColor($r('app.color.text_secondary'))
              .margin({ bottom: 16 })
            Row({ space: 12 }) {
              Button('重新扫描')
                .backgroundColor($r('app.color.control_bg'))
                .onClick(() => {
                  const added = this.lib.scanShortcuts();
                  promptAction.showToast({ message: added > 0 ? `新增 ${added} 个应用` : '没有发现新快捷方式' });
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
        }
        .width('100%').height('100%')
        .justifyContent(FlexAlign.Center)
        .backgroundColor($r('app.color.window_bg'))
      } else if (this.bp == 'lg') {
        // lg 大卡栅格 (Pad 全屏 / PC 宽窗): 五列 1fr, 卡片含图标/名称/路径副标题
        Grid() {
          ForEach(this.filtered(), (entry: AppEntry) => {
            GridItem() {
              Stack() {
                Column() {
                  // 启动反馈加载态: 点击后转圈直至窗口弹出/失败/超时
                  if (this.launchingPath == entry.path) {
                    LoadingProgress()
                      .width(36).height(36)
                      .color($r('app.color.accent'))
                  } else if (entry.icon) {
                    Image(`file://${entry.icon}`)
                      .width(40).height(40)
                  } else {
                    Text('▶')
                      .fontSize(32)
                      .fontColor($r('app.color.accent'))
                  }
                  Text(entry.name)
                    .fontColor($r('app.color.accent_soft'))
                    .margin({ top: 8 })
                    .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                  Text(entry.path)
                    .fontSize(10)
                    .fontColor($r('app.color.text_tertiary'))
                    .margin({ top: 2 })
                    .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                }
                .width('100%').height('100%')
                .justifyContent(FlexAlign.Center)
                .alignItems(HorizontalAlign.Center)
                // 内建角标
                if (entry.builtin) {
                  Text('内建')
                    .fontSize(10)
                    .fontColor($r('app.color.text_tertiary'))
                    .padding({ left: 6, right: 6, top: 2, bottom: 2 })
                    .backgroundColor($r('app.color.control_bg'))
                    .borderRadius(4)
                    .margin(6)
                }
              }
              .alignContent(Alignment.TopEnd)
              .width('100%').height(120)
              .backgroundColor($r('app.color.card_bg'))
              .borderRadius(8)
              .onClick(() => this.launch(entry))
              .hoverEffect(HoverEffect.Highlight)
              .bindContextMenu(this.entryMenu(entry), ResponseType.LongPress)
            }
          }, (entry: AppEntry) => entry.path)
        }
        .columnsTemplate('1fr 1fr 1fr 1fr 1fr')
        .columnsGap(12)
        .rowsGap(12)
        .padding(12)
        .width('100%').height('100%')
        .backgroundColor($r('app.color.window_bg'))
      } else {
        // sm/md 列表
        List({ space: 1 }) {
          ForEach(this.filtered(), (entry: AppEntry) => {
            ListItem() {
              Row() {
                if (this.launchingPath == entry.path) {
                  LoadingProgress()
                    .width(22).height(22)
                    .color($r('app.color.accent'))
                    .margin({ right: 6 })
                } else if (entry.icon) {
                  Image(`file://${entry.icon}`)
                    .width(24).height(24).margin({ right: 6 })
                } else {
                  Text('▶').width(28)
                }
                Text(entry.name)
                  .fontColor($r('app.color.accent_soft'))
                  .layoutWeight(1)
                  .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                if (entry.builtin) {
                  Text('内建')
                    .fontSize(10)
                    .fontColor($r('app.color.text_tertiary'))
                    .padding({ left: 6, right: 6, top: 2, bottom: 2 })
                    .backgroundColor($r('app.color.control_bg'))
                    .borderRadius(4)
                }
              }
              .width('100%').height(52).padding({ left: 14, right: 14 })
              .alignItems(VerticalAlign.Center)
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
  }
}
