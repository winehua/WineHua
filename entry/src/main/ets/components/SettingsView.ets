import { promptAction, SegmentButton, SegmentButtonOptions, window } from '@kit.ArkUI';
import { fileUri, picker } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { WineEnvService } from '../service/WineEnvService';
import { FontImportService } from '../service/FontImportService';
import { LogExportService } from '../service/LogExportService';
import { DisplaySettingsService } from '../service/DisplaySettingsService';
import { ThemeService, THEME_SYSTEM, THEME_LIGHT, THEME_DARK } from '../service/ThemeService';
import { InputSettingsService } from '../service/InputSettingsService';
import { DesktopModeService, MODE_VIRTUAL, MODE_FUSION, isSubWindowFormFactor } from '../service/DesktopModeService';
import { compatIndexOfMode, compatModeFromIndex } from '../service/Box64Dynarec';

// 设置页 (设计 4.6, 环境已并入): 显示 (置顶) / 环境管理 (Wine 引擎 · Windows 字体 ·
// 会话维护 · 危险区)。原环境页 (EnvPanel) 三卡整体并入本页; 原"关于"卡片与
// "开发者/系统信息"折叠区已摘出为独立导航页 (AboutView), 主界面导航为
// 应用库/运行中/文件/设置/关于 五项。
// 卡片外观由外层 Column 承担。
// 说明: 应用调色板分两套 — base=浅色 / dark=深色 (resources/*/element/
// color.json), 外观三档 (跟随系统/浅色/深色) 由 ThemeService 下发生效,
// 只覆盖应用自身界面; Wine 内的程序窗口不受影响。
// DPI 缩放用 Slider 交互 (同悬浮条"光标速度"), 全局生效, 重启应用后生效
// (见 DisplaySettingsService); 输入设置在桌面悬浮条与布局编辑里 (设计 P4/P5 已落地)。
@Component
export struct SettingsView {
  // -- 环境状态镜像 (订阅驱动, 原 EnvPanel) --
  @State state: string = 'idle';
  @State sessionAlive: boolean = false;
  @State busy: boolean = false;
  @State prefixReady: boolean = false;
  @State statusText: string = '检测中...';
  // 渲染策略镜像 (dxvk_legacy / dxvk_modern_2_6 / vkd3d_limited_500k / wined3d);
  // 默认 DXVK 1.10 (legacy); 切换后直启即生效, 已运行会话内需重启会话
  @State d3dBackend: string = 'dxvk_legacy';
  // SegmentButton 选中项 (@Link 双向): 0=DXVK1.10 1=DXVK2.6 2=VKD3D 3=WineD3D,
  // 与 d3dBackend 同步
  @State backendSel: number[] = [0];
  // Wine 语言分段选中项: 0=中文(zh_CN) 1=English(en_US), 与 wineLang 同步
  @State langSel: number[] = [0];
  @State wineLang: string = 'zh_CN';
  @State restartHint: boolean = false;
  @State langRestartHint: boolean = false;
  // 兼容模式分段选中项 (Box64 dynarec 全局档位, 镜像 env.getCompatMode):
  // 0=默认(出厂基线) 1=稳定 2=保守 3=均衡 4=性能 (选项对齐自定义启动弹窗)
  @State compatSel: number[] = [0];
  @State compatMode: string = '';
  @State compatRestartHint: boolean = false;
  // 桌面模式分段选中项: 0=多窗口/融合 1=虚拟桌面 (由 mirror 按服务模式纠正)
  @State desktopModeSel: number[] = [0];
  @State desktopMode: string = MODE_FUSION;
  @State desktopModeRestartHint: boolean = false;
  // 字体导入进行中 (镜像 WineEnvService.fontImporting): 按钮使能 + 文案
  @State fontImporting: boolean = false;
  // -- 显示 --
  @State scaleText: string = '';
  // 全屏游戏鼠标速度 (InputSettingsService.relMouseSpeed, 立即生效不需重启)
  @State relSpeedText: string = '100%';
  // 保持屏幕常亮 (镜像 DisplaySettingsService, 设置页即时生效)
  @State keepScreenOn: boolean = false;
  // 外观三档 (镜像 ThemeService): 0=跟随系统 1=浅色 2=深色
  // 初值取服务默认档 (跟随系统), aboutToAppear 里按读盘值纠正
  @State themeSel: number[] = [0];
  @State themeMode: string = THEME_SYSTEM;

  private env: WineEnvService = WineEnvService.getInstance();
  private displaySettings: DisplaySettingsService = DisplaySettingsService.getInstance();
  private inputSettings: InputSettingsService = InputSettingsService.getInstance();

  private mirror = (): void => {
    this.state = this.env.engineState;
    this.sessionAlive = this.env.sessionAlive;
    this.busy = this.env.isBusy();
    this.prefixReady = this.env.prefixReady;
    this.statusText = this.env.statusText;
    this.d3dBackend = this.env.getD3dBackend();
    // 派生选中项; 仅在值变化时赋值, 避免 notify 覆盖用户刚点击的 @Link 状态
    const sel = this.d3dBackend == 'dxvk_legacy' ? 0 :
      this.d3dBackend == 'dxvk_modern_2_6' ? 1 :
        this.d3dBackend == 'vkd3d_limited_500k' ? 2 : 3;
    if (this.backendSel[0] != sel) {
      this.backendSel = [sel];
    }
    this.wineLang = this.env.getWineLang();
    const langSel = this.wineLang == 'en_US' ? 1 : 0;
    if (this.langSel[0] != langSel) {
      this.langSel = [langSel];
    }
    this.compatMode = this.env.getCompatMode();
    // 派生兼容模式选中项 (变化才赋值, 与 backendSel 同守卫; 未知值兜底 0,
    // 与 normalizeCompatMode 同向 — UI 高亮与不注入的实际行为一致)
    const compatIdx = compatIndexOfMode(this.compatMode);
    if (this.compatSel[0] != compatIdx) {
      this.compatSel = [compatIdx];
    }
    // 桌面模式镜像 (默认值见 DesktopModeService.DEFAULT_MODE)
    const desktopMode = DesktopModeService.getInstance().mode;
    if (desktopMode != this.desktopMode) {
      this.desktopMode = desktopMode;
      const modeSel = desktopMode == MODE_FUSION ? 0 : 1;
      if (this.desktopModeSel[0] != modeSel) {
        this.desktopModeSel = [modeSel];
      }
    }
    this.fontImporting = FontImportService.getInstance().fontImporting;
  }

  private fmtScale(v: number): string {
    return `${Math.round(v * 100) / 100}x`;
  }

  aboutToAppear(): void {
    this.env.subscribe(this.mirror);
    // 字体导入状态独立订阅 (FontImportService 拆分后不再经 env notify)
    FontImportService.getInstance().subscribe(this.mirror);
    this.mirror();
    this.scaleText = this.fmtScale(this.displaySettings.scale);
    this.keepScreenOn = this.displaySettings.keepScreenOn;
    this.themeMode = ThemeService.getInstance().mode;
    this.themeSel = [this.themeIndexOf(this.themeMode)];
    this.relSpeedText = `${Math.round(this.inputSettings.relMouseSpeed * 100)}%`;
  }

  // 保持屏幕常亮: 持久化后统一走 env 策略 — wine 初始化中强制常亮,
  // 开关只改设置值不生效常亮状态 (防打穿); 非初始化期立即应用到主窗口
  // (env 内 WineWindowManager.getMainWindow, 不依赖 getLastWindow —
  // 多窗口下它会返回游戏子窗口)
  private toggleKeepScreenOn(v: boolean): void {
    this.keepScreenOn = v;
    this.displaySettings.setKeepScreenOn(v);
    this.env.applyKeepScreenOn();
  }

  // 状态文本配色, 与标题栏同一套规则: 失败红 / 运行绿 / 就绪或进行中蓝 / 未启动灰
  private statusColor(): Resource {
    if (this.state == 'failed') {
      return $r('app.color.danger');
    }
    if (this.state == 'ready') {
      return this.sessionAlive ? $r('app.color.state_ok') : $r('app.color.state_info');
    }
    if (this.state == 'starting' || this.state == 'stopping' || this.state == 'ready-degraded') {
      return $r('app.color.state_info');
    }
    return $r('app.color.text_tertiary');
  }

  // 渲染策略分段控件: 系统预置 SegmentButton 胶囊单选 (替代手写双 Button 行);
  // 切换仅写配置 (service 内持久化); 直启程序逐进程注入新 env 立即生效,
  // 已运行的桌面会话内程序继承旧 env, 需重启会话
  private selectBackend(backend: string): void {
    if (backend == this.d3dBackend) {
      return;
    }
    this.env.setD3dPreference(backend);
    this.restartHint = true;
  }

  // 兼容模式档位切换: 仅写配置 (service 内持久化)。程序级生效:
  // 新启动的程序 (launch/openInWine 直启) 立即生效;
  // 会话启动链 (wineserver/wineboot/explorer) 不带档位, 无重启语义
  private selectCompat(mode: string): void {
    if (mode == this.compatMode) {
      return;
    }
    this.env.setCompatModePreference(mode);
    this.compatRestartHint = true;
  }

  // Wine 语言切换: 同渲染策略, 仅写配置, 重启 Wine 引擎后生效
  private selectLang(lang: string): void {
    if (lang == this.wineLang) {
      return;
    }
    this.env.setWineLangPreference(lang);
    this.langRestartHint = true;
  }

  // 桌面模式切换 (仅 PC): 写配置, 重启 Wine 引擎后生效 (explorer 是否启动/
  // 合成器策略在引擎启动时按此值判定, 运行中切换不影响当前会话)
  private selectDesktopMode(mode: string): void {
    if (mode == this.desktopMode) {
      return;
    }
    DesktopModeService.getInstance().setMode(mode);
    this.desktopMode = mode;
    this.desktopModeRestartHint = true;
  }

  // 外观档位 → 分段索引 (0=跟随系统 1=浅色 2=深色)
  private themeIndexOf(mode: string): number {
    if (mode == THEME_LIGHT) {
      return 1;
    }
    if (mode == THEME_DARK) {
      return 2;
    }
    return 0;
  }

  // 外观切换: 落盘并立即生效 (setColorMode 后系统重新解析 $r('app.color.*')),
  // 无需重启应用 — 与 DPI 缩放/语言不同, 后者是 Wine 侧配置要重启引擎
  private selectTheme(mode: string): void {
    if (mode == this.themeMode) {
      return;
    }
    this.themeMode = mode;
    ThemeService.getInstance().setMode(mode);
  }

  private confirmReset(): void {
    AlertDialog.show({
      title: '重置 Wine 引擎',
      message: '将清空整个 Wine 容器（包括所有已安装程序和存档），且不可恢复。确定继续？',
      primaryButton: {
        value: '取消',
        action: () => {}
      },
      secondaryButton: {
        value: '重置',
        fontColor: $r('app.color.danger'),
        action: () => {
          this.env.doReset();
        }
      }
    });
  }

  private confirmStopAll(): void {
    AlertDialog.show({
      title: '强制停止 Wine 引擎',
      message: '将结束所有正在运行的 Windows 程序（未保存数据会丢失）。确定继续？',
      primaryButton: {
        value: '取消',
        action: () => {}
      },
      secondaryButton: {
        value: '停止全部',
        fontColor: $r('app.color.danger'),
        action: () => {
          this.env.stopAllSessions();
          promptAction.showToast({ message: '正在停止全部会话...' });
        }
      }
    });
  }

  private doExportLogs(): void {
    const path = LogExportService.getInstance().exportLogs(this.env);
    promptAction.showToast({
      message: path ? `日志已导出:\n${path}` : '日志导出失败'
    });
  }

  // Windows 字体导入: 用户把 C:\Windows\Fonts 打包为 zip 选入, 解压拷入
  // prefix 的 windows\Fonts (实现在 service; 全程全窗遮罩, 见 Index 根 Stack)。
  // 后缀不在 picker 侧过滤 (同文件页"导入"的教训: 系统筛选有翻车风险), 选中后校验
  private pickFontsZip(): void {
    const docPicker = new picker.DocumentViewPicker(getContext(this));
    docPicker.select({ maxSelectNumber: 1 }).then((uris: string[]) => {
      if (!uris || uris.length == 0) {
        return;
      }
      let name = '';
      try {
        name = new fileUri.FileUri(uris[0]).name;
      } catch (_) { /* 名字取不到不挡路, 交给 service 按内容校验 */ }
      if (name != '' && !name.toLowerCase().endsWith('.zip')) {
        promptAction.showToast({ message: '请选择 zip 格式的字体包' });
        return;
      }
      FontImportService.getInstance().importFontsZip(uris[0]).then((msg: string) => {
        promptAction.showToast({ message: msg });
      });
    }).catch((e: Object) => {
      hilog.error(0x0000, 'SettingsView', 'fonts picker fail: %{public}s', JSON.stringify(e));
    });
  }

  build() {
    Scroll() {
      Column({ space: 14 }) {
        // ══ 卡 1: 显示 (置顶) ══
        // 间距规范 (本页统一): 项内 4 / 并列控件之间 8 / 项之间 16 / 卡片之间 14。
        // 项内必须明显小于项之间 — Checkbox/Slider/SegmentButton 自带高度会占掉
        // 视觉间距, 若与项之间同值, 说明文字看着会比下一项的标题还远 (分组感反了)。
        Column({ space: 16 }) {
          Text('显示')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .width('100%')
          Column({ space: 4 }) {
            Row() {
              Text('DPI 缩放')
                .fontColor($r('app.color.text_primary'))
                .layoutWeight(1)
              Text(this.scaleText)
                .fontColor($r('app.color.text_tertiary'))
            }
            .width('100%')
            // Slider 交互 (同悬浮条"光标速度"): 拖动中只更新显示值 (即时预览),
            // 松手/点按才落盘, 避免拖动过程频繁写 preferences
            Slider({ value: this.displaySettings.scale, min: 1.0, max: 3.0, step: 0.25 })
              .onChange((value: number, mode: SliderChangeMode) => {
                this.scaleText = this.fmtScale(value);
                if (mode != SliderChangeMode.Moving) {
                  this.displaySettings.setScale(value);
                }
              })
            Text('缩放调节重启应用后生效')
              .fontSize(12)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
          }
          .width('100%')

          // 保持屏幕常亮 (参照 HiSH): 窗口级 keepScreenOn 标志, 主窗口/游戏
          // 子窗口启动时各应用一次 (EntryAbility/WineWindowAbility), 切换即时生效
          Column({ space: 4 }) {
            Row() {
              Text('保持屏幕常亮')
                .fontColor($r('app.color.text_primary'))
                .layoutWeight(1)
              Checkbox()
                .select(this.keepScreenOn)
                .onChange((v: boolean) => this.toggleKeepScreenOn(v))
            }
            .width('100%').alignItems(VerticalAlign.Center)
            Text('开启后屏幕不会自动熄灭（运行 Windows 程序 / 挂机时适用）')
              .fontSize(12)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
          }
          .width('100%')

          // 外观 (三档, 立即生效): 只影响应用自身界面 — Wine 里的程序窗口
          // 由 Wine 自己绘制 (explorer/comctl32), 不受此设置影响
          Column({ space: 4 }) {
            Text('外观')
              .fontColor($r('app.color.text_primary'))
              .width('100%')
            SegmentButton({
              options: SegmentButtonOptions.capsule({
                buttons: [{ text: '跟随系统' }, { text: '浅色' }, { text: '深色' }],
                multiply: false
              }),
              selectedIndexes: $themeSel,
              onItemClicked: (index: number) => {
                this.selectTheme(index == 0 ? THEME_SYSTEM :
                  (index == 1 ? THEME_LIGHT : THEME_DARK));
              }
            })
            .width('100%')
            Text('只影响应用界面（启动器/设置/应用库）；Wine 里的程序窗口不受影响')
              .fontSize(12)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
          }
          .width('100%')

          // Wine 语言 (从 Wine 引擎卡移入, 同属显示/区域设置):
          // 切换仅写配置, 重启 Wine 引擎后生效
          Column({ space: 4 }) {
            Text('Wine 语言')
              .fontColor($r('app.color.text_primary'))
              .width('100%')
            SegmentButton({
              options: SegmentButtonOptions.capsule({
                buttons: [{ text: '中文' }, { text: 'English' }],
                multiply: false
              }),
              selectedIndexes: $langSel,
              onItemClicked: (index: number) => {
                this.selectLang(index == 0 ? 'zh_CN' : 'en_US');
              }
            })
            .width('100%')
            Text('影响 Windows 程序的界面语言与区域格式')
              .fontSize(12)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
            // 重启提示属于本项 (同一设置的另一条补充说明), 故留在项内
            if (this.langRestartHint) {
              Text('语言已更改, 重启 Wine 引擎后生效')
                .fontSize(12)
                .fontColor($r('app.color.state_warn'))
                .width('100%')
            }
          }
          .width('100%')

          // 桌面模式 (全形态可切): 多窗口/融合 = 每个程序独立 OHOS 窗口
          // (可拖动/缩放/置顶); 虚拟桌面 = Wine 桌面合成为单一窗口
          Column({ space: 4 }) {
            Text('桌面模式')
              .fontColor($r('app.color.text_primary'))
              .width('100%')
            SegmentButton({
              options: SegmentButtonOptions.capsule({
                // 移动形态叫"多窗口模式", PC 沿用旧名"融合模式"
                buttons: [{
                  text: isSubWindowFormFactor() ? '多窗口模式' : '融合模式'
                }, { text: '虚拟桌面' }],
                multiply: false
              }),
              selectedIndexes: $desktopModeSel,
              onItemClicked: (index: number) => {
                this.selectDesktopMode(index == 0 ? MODE_FUSION : MODE_VIRTUAL);
              }
            })
            .width('100%')
            // 说明文字: 多窗口仍是试验功能, 加标注
            Text(isSubWindowFormFactor() ?
              '虚拟桌面: Wine 桌面在独立窗口运行; 多窗口模式: 每个程序独立窗口（试验功能）' :
              '虚拟桌面: Wine 桌面在独立窗口运行; 融合模式: 每个程序独立窗口')
              .fontSize(12)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
            if (this.desktopModeRestartHint) {
              Text('桌面模式已更改, 重启 Wine 引擎后生效')
                .fontSize(12)
                .fontColor($r('app.color.state_warn'))
                .width('100%')
            }
          }
          .width('100%')
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(12)

        // ══ 卡 1.5: 输入 ══
        Column({ space: 16 }) {
          Text('输入')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .width('100%')
          Column({ space: 4 }) {
            Row() {
              Text('全屏游戏鼠标速度')
                .fontColor($r('app.color.text_primary'))
                .layoutWeight(1)
              Text(this.relSpeedText)
                .fontColor($r('app.color.text_tertiary'))
            }
            .width('100%')
            // Slider 交互同 DPI 缩放: 拖动中只更新显示值, 松手/点按才落盘;
            // 与 DPI 不同 — 倍率经 InputDeviceMapper.getScale 每事件直读,
            // 立即生效, 无需重启
            Slider({ value: this.inputSettings.relMouseSpeed, min: 0.25, max: 4.0, step: 0.25 })
              .onChange((value: number, mode: SliderChangeMode) => {
                this.relSpeedText = `${Math.round(value * 100)}%`;
                this.inputSettings.setRelMouseSpeed(value, mode != SliderChangeMode.Moving);
              })
            Text('适用于游戏内置的鼠标光标、旋转视角等场景，立即生效')
              .fontSize(12)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
          }
          .width('100%')
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(12)

        // ══ 卡 2: Wine 引擎 (原环境页) ══
        Column({ space: 16 }) {
          Row() {
            Circle({ width: 8, height: 8 })
              .fill(this.prefixReady ? $r('app.color.state_ok') : $r('app.color.state_warn'))
              .margin({ right: 6 })
            Text('Wine 引擎')
              .fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.text_primary'))
              .layoutWeight(1)
            Text(this.statusText)
              .fontSize(13)
              .fontColor(this.statusColor())
              .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
          }
          .width('100%').alignItems(VerticalAlign.Center)

          Column({ space: 4 }) {
            Text('渲染策略')
              .fontColor($r('app.color.text_primary'))
              .width('100%')
            SegmentButton({
              options: SegmentButtonOptions.capsule({
                buttons: [{ text: 'DXVK 1.10' }, { text: 'DXVK 2.6' }, { text: 'VKD3D' }, { text: 'WineD3D' }],
                multiply: false
              }),
              selectedIndexes: $backendSel,
              onItemClicked: (index: number) => {
                this.selectBackend(index == 0 ? 'dxvk_legacy' :
                  index == 1 ? 'dxvk_modern_2_6' :
                    index == 2 ? 'vkd3d_limited_500k' : 'wined3d');
              }
            })
            .width('100%')
            Text('DXVK 性能好 (1.10 稳定 / 2.6 新版); D3D12 程序选 VKD3D; 个别程序异常时可试 WineD3D')
              .fontSize(12)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
            if (this.restartHint) {
              Text('渲染策略已更改, 之后启动的程序立即生效; 桌面会话内的程序需重启会话')
                .fontSize(12)
                .fontColor($r('app.color.state_warn'))
                .width('100%')
            }
          }
          .width('100%')

          Column({ space: 4 }) {
            Text('兼容模式')
              .fontColor($r('app.color.text_primary'))
              .width('100%')
            SegmentButton({
              options: SegmentButtonOptions.capsule({
                buttons: [{ text: '默认' }, { text: '稳定' }, { text: '保守' },
                  { text: '均衡' }, { text: '性能' }],
                multiply: false
              }),
              selectedIndexes: $compatSel,
              onItemClicked: (index: number) => {
                this.selectCompat(compatModeFromIndex(index) ?? '');
              }
            })
            .width('100%')
            Text('性能档最快 / 稳定档最稳; 个别程序异常时可降档重试')
              .fontSize(12)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
            if (this.compatRestartHint) {
              Text('兼容模式已更改; 仅对之后启动的程序生效')
                .fontSize(12)
                .fontColor($r('app.color.state_warn'))
                .width('100%')
            }
          }
          .width('100%')
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(8)

        // ══ 卡 3: Windows 字体 ══
        Column({ space: 16 }) {
          Text('Windows 字体')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .width('100%')
          Column({ space: 4 }) {
            Text('将 Windows 的 C:\\Windows\\Fonts 目录打包为 zip 后导入，解决程序缺字体、显示方块的问题（导入前会校验包内容确为 Fonts 目录备份）')
              .fontSize(12)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
            Button(this.fontImporting ? '正在导入...' : '选择 zip 字体包导入')
              .width('100%').backgroundColor($r('app.color.control_bg'))
              .fontColor($r('app.color.text_primary'))
              // 需 prefix 已就绪 (目标目录在 drive_c 下); 导入中防重入
              .enabled(this.prefixReady && !this.fontImporting)
              .onClick(() => this.pickFontsZip())
            Text('导入后新启动的 Windows 程序即可使用，运行中的程序需重启')
              .fontSize(12)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
          }
          .width('100%')
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(8)

        // ══ 卡 4: 会话维护 (原环境页) ══
        Column({ space: 16 }) {
          Text('会话维护')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .width('100%')
          // 三个按钮是并列控件 (无标签/说明的层级), 用 8 而非项内 4
          Column({ space: 8 }) {
            Button((this.sessionAlive || this.state == 'ready' || this.state == 'ready-degraded') ? '重启引擎' : '启动引擎')
              .width('100%').backgroundColor($r('app.color.action_primary'))
              // 判据 = 状态机: 仅 空闲/就绪/降级/失败 可进入; 会话存活时走
              // restartEngine 编排 (先停后启), 不再直接 doInit 双开桌面。
              // ready-degraded 必须可重启: 桌面一直没起来时这是用户自救入口
              .enabled(!this.busy &&
                (this.state == 'idle' || this.state == 'ready' || this.state == 'failed' ||
                  this.state == 'ready-degraded'))
              .onClick(() => {
                this.restartHint = false;
                this.langRestartHint = false;
                this.desktopModeRestartHint = false;
                this.compatRestartHint = false;
                this.env.restartEngine();
              })
            Button('强制停止 Wine 引擎')
              .width('100%').backgroundColor($r('app.color.action_warning'))
              // 判据 = 会话存活 (桌面 root 或用户进程), 与 prefix 文件就绪无关
              .enabled(this.sessionAlive && !this.busy)
              .onClick(() => this.confirmStopAll())
            Button('导出日志')
              .width('100%').backgroundColor($r('app.color.control_bg'))
              .fontColor($r('app.color.text_primary'))
              .onClick(() => this.doExportLogs())
          }
          .width('100%')
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(8)

        // ══ 卡 5: 危险区 (红色, 原环境页) ══
        Column({ space: 16 }) {
          Text('危险区')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.danger'))
            .width('100%')
          Column({ space: 4 }) {
            Text('重置会删除容器内全部应用数据, 操作不可恢复')
              .fontSize(12)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
            Button('重置 Wine 引擎')
              .width('100%').backgroundColor($r('app.color.action_danger'))
              .fontColor($r('app.color.danger'))
              // 编排内含"先停会话再删 prefix", 任何非忙状态都可用
              .enabled(!this.busy)
              .onClick(() => this.confirmReset())
          }
          .width('100%')
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(8)
        .border({ width: 1, color: $r('app.color.action_danger') })

      }
      .width('100%')
      .padding(14)
    }
    .width('100%').height('100%')
    .scrollBar(BarState.Off)
    .backgroundColor($r('app.color.window_bg'))
  }
}
