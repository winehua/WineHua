import { bundleManager } from '@kit.AbilityKit';
import { deviceInfo } from '@kit.BasicServicesKit';
import { ComposeListItem, promptAction, SegmentButton, SegmentButtonOptions } from '@kit.ArkUI';
import { fileUri, picker } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { WineEnvService } from '../service/WineEnvService';
import { DisplaySettingsService } from '../service/DisplaySettingsService';

const WINE_PREFIX = '/data/storage/el2/base/files/.wine';

// 设置页 (设计 4.6, 环境已并入): 显示 (置顶) / 环境管理 (Wine 引擎 · Windows 字体 ·
// 会话维护 · 危险区) / 开发者折叠 / 关于。原环境页 (EnvPanel) 三卡 + 开发者折叠区
// 整体并入本页, 主界面导航收敛为 应用库/运行中/文件/设置 四项。
// 行用系统预置 ComposeListItem (左标题+右值), 统一行高/间距/字级。
// 注: ComposeListItem 不宜挂通用属性 (会落到额外生成的 __Common__ 节点上),
// 故卡片外观由外层 Column 承担。
// 说明: 应用调色板目前只有深色一套, 主题切换后续版本开放。
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
  @State zHome: string = '';
  // 渲染策略镜像 (dxvk_legacy / wined3d); 切换后提示需重启引擎才生效
  @State d3dBackend: string = 'dxvk_legacy';
  // SegmentButton 选中项 (@Link 双向): 0=dxvk_legacy 1=wined3d, 与 d3dBackend 同步
  @State backendSel: number[] = [0];
  // Wine 语言分段选中项: 0=中文(zh_CN) 1=English(en_US), 与 wineLang 同步
  @State langSel: number[] = [0];
  @State wineLang: string = 'zh_CN';
  @State restartHint: boolean = false;
  @State langRestartHint: boolean = false;
  // 字体导入进行中 (镜像 WineEnvService.fontImporting): 按钮使能 + 文案
  @State fontImporting: boolean = false;
  @State devExpanded: boolean = false;
  @State perfProfile: string = 'baseline';
  // -- 显示/关于 --
  @State versionName: string = '';
  @State scaleText: string = '';

  private env: WineEnvService = WineEnvService.getInstance();
  private displaySettings: DisplaySettingsService = DisplaySettingsService.getInstance();

  private mirror = (): void => {
    this.state = this.env.engineState;
    this.sessionAlive = this.env.sessionAlive;
    this.busy = this.env.isBusy();
    this.prefixReady = this.env.prefixReady;
    this.statusText = this.env.statusText;
    this.zHome = this.env.zHome;
    this.d3dBackend = this.env.getD3dBackend();
    // 派生选中项; 仅在值变化时赋值, 避免 notify 覆盖用户刚点击的 @Link 状态
    const sel = this.d3dBackend == 'dxvk_legacy' ? 0 : 1;
    if (this.backendSel[0] != sel) {
      this.backendSel = [sel];
    }
    this.wineLang = this.env.getWineLang();
    const langSel = this.wineLang == 'en_US' ? 1 : 0;
    if (this.langSel[0] != langSel) {
      this.langSel = [langSel];
    }
    this.perfProfile = this.env.getPerfProfile();
    this.fontImporting = this.env.fontImporting;
  }

  private fmtScale(v: number): string {
    return `${Math.round(v * 100) / 100}x`;
  }

  aboutToAppear(): void {
    this.env.subscribe(this.mirror);
    this.mirror();
    this.scaleText = this.fmtScale(this.displaySettings.scale);
    bundleManager.getBundleInfoForSelf(
      bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION
    ).then((info: bundleManager.BundleInfo) => {
      this.versionName = info.versionName;
    }).catch(() => {
      this.versionName = '';
    });
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
  // 切换仅写配置 (service 内持久化), 重启 Wine 引擎后生效
  private selectBackend(backend: string): void {
    if (backend == this.d3dBackend) {
      return;
    }
    this.env.setD3dPreference(backend);
    this.restartHint = true;
  }

  // Wine 语言切换: 同渲染策略, 仅写配置, 重启 Wine 引擎后生效
  private selectLang(lang: string): void {
    if (lang == this.wineLang) {
      return;
    }
    this.env.setWineLangPreference(lang);
    this.langRestartHint = true;
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
    const path = this.env.exportLogs();
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
      this.env.importFontsZip(uris[0]).then((msg: string) => {
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
        Column({ space: 8 }) {
          Text('显示')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .width('100%')
          Row() {
            Text('DPI 缩放 (全局)')
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

          // Wine 语言 (从 Wine 引擎卡移入, 同属显示/区域设置):
          // 切换仅写配置, 重启 Wine 引擎后生效
          Text('Wine 语言')
            .fontSize(13)
            .fontColor($r('app.color.text_secondary'))
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
          if (this.langRestartHint) {
            Text('语言已更改, 重启 Wine 引擎后生效')
              .fontSize(12)
              .fontColor($r('app.color.state_warn'))
              .width('100%')
          }
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(12)

        // ══ 卡 2: Wine 引擎 (原环境页) ══
        Column({ space: 10 }) {
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

          Text('渲染策略')
            .fontSize(13)
            .fontColor($r('app.color.text_secondary'))
            .width('100%')
          SegmentButton({
            options: SegmentButtonOptions.capsule({
              buttons: [{ text: 'DXVK (推荐)' }, { text: 'WineD3D' }],
              multiply: false
            }),
            selectedIndexes: $backendSel,
            onItemClicked: (index: number) => {
              this.selectBackend(index == 0 ? 'dxvk_legacy' : 'wined3d');
            }
          })
          .width('100%')
          Text('DXVK 性能更好; 个别程序异常时可试 WineD3D')
            .fontSize(12)
            .fontColor($r('app.color.text_tertiary'))
            .width('100%')
          if (this.restartHint) {
            Text('渲染策略已更改, 重启 Wine 引擎后生效')
              .fontSize(12)
              .fontColor($r('app.color.state_warn'))
              .width('100%')
          }
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(8)

        // ══ 卡 3: Windows 字体 ══
        Column({ space: 10 }) {
          Text('Windows 字体')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .width('100%')
          Text('将 Windows 的 C:\\Windows\\Fonts 目录打包为 zip 后导入，解决程序缺字体、显示方块的问题（导入前会校验包内容确为 Fonts 目录备份）')
            .fontSize(12)
            .fontColor($r('app.color.text_tertiary'))
            .width('100%')
          Button(this.fontImporting ? '正在导入...' : '选择 zip 字体包导入')
            .width('100%').backgroundColor($r('app.color.control_bg'))
            // 需 prefix 已就绪 (目标目录在 drive_c 下); 导入中防重入
            .enabled(this.prefixReady && !this.fontImporting)
            .onClick(() => this.pickFontsZip())
          Text('导入后新启动的 Windows 程序即可使用，运行中的程序需重启')
            .fontSize(12)
            .fontColor($r('app.color.text_tertiary'))
            .width('100%')
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(8)

        // ══ 卡 4: 会话维护 (原环境页) ══
        Column({ space: 10 }) {
          Text('会话维护')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .width('100%')
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
              this.env.restartEngine();
            })
          Button('强制停止 Wine 引擎')
            .width('100%').backgroundColor($r('app.color.action_warning'))
            // 判据 = 会话存活 (桌面 root 或用户进程), 与 prefix 文件就绪无关
            .enabled(this.sessionAlive && !this.busy)
            .onClick(() => this.confirmStopAll())
          Button('导出日志')
            .width('100%').backgroundColor($r('app.color.control_bg'))
            .onClick(() => this.doExportLogs())
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(8)

        // ══ 卡 5: 危险区 (红色, 原环境页) ══
        Column({ space: 10 }) {
          Text('危险区')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.danger'))
            .width('100%')
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
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(8)
        .border({ width: 1, color: $r('app.color.action_danger') })

        // ══ 开发者 (折叠, 原环境页折叠区 + 原设置页诊断行去重合并) ══
        Column() {
          Row() {
            Text('开发者')
              .fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.text_secondary'))
              .layoutWeight(1)
            SymbolGlyph(this.devExpanded ? $r('sys.symbol.chevron_down') : $r('sys.symbol.chevron_right'))
              .fontSize(16)
              .fontColor([$r('app.color.text_secondary')])
          }
          .width('100%')
          .padding({ top: 4, bottom: 4 })
          .onClick(() => {
            this.devExpanded = !this.devExpanded;
          })
          if (this.devExpanded) {
            Text(`诊断 profile: ${this.perfProfile}`)
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 6 })
            Text(`渲染后端: ${this.d3dBackend}`)
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 4 })
            Text(`Prefix: ${WINE_PREFIX}`)
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 4 })
            Text(`Z: 映射: ${this.zHome}`)
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 4 })
            Text(`设备类型: ${deviceInfo.deviceType}`)
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 4 })
            Text('自动化测试经 hdc 以 want 参数启动 (winehua.mode=smoke)')
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 8 })
          }
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(8)

        // ══ 关于 ══
        Column({ space: 8 }) {
          Text('关于')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .width('100%')
          ComposeListItem({
            contentItem: { primaryText: '万花' },
            operateItem: { text: this.versionName ? `v${this.versionName}` : '' }
          })
          Text('开源组件')
            .fontColor($r('app.color.text_secondary'))
            .width('100%')
            .margin({ top: 4 })
          Text('Wine · Box64 · DXVK · VirGL Renderer · Mesa')
            .fontSize(12)
            .fontColor($r('app.color.text_tertiary'))
            .width('100%')
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(12)
      }
      .width('100%')
      .padding(14)
    }
    .width('100%').height('100%')
    .scrollBar(BarState.Off)
    .backgroundColor($r('app.color.window_bg'))
  }
}
