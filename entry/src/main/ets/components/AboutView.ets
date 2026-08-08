import { bundleManager, common } from '@kit.AbilityKit';
import { deviceInfo } from '@kit.BasicServicesKit';
import { statfs } from '@kit.CoreFileKit';
import { display } from '@kit.ArkUI';
import { WineEnvService } from '../service/WineEnvService';

const WINE_PREFIX = '/data/storage/el2/base/files/.wine';

// 关于页 (主界面第 5 导航 tab): 万花 (WineHua) 功能简介 + 英文名 + 版本号 /
// 开源组件 / 开源仓库 / 贡献者 / 系统信息。原设置页尾部小"关于"卡片摘出并扩充
// (见 SettingsView 注释); "系统信息"折叠区也整体从设置页搬入并丰富设备信息,
// 便于用户反馈问题时做诊断。
// 卡片风格与设置页一致 (Column 卡片 + card_bg + 圆角)。
// 版本号取 bundle 的 versionName; 贡献者/仓库链接点击经 openLink 拉起系统浏览器
// (无浏览器等失败静默忽略, 链接文本始终可读兜底)。
@Component
export struct AboutView {
  @State versionName: string = '';
  // -- 系统信息 (诊断用) --
  @State perfProfile: string = 'baseline';
  @State d3dBackend: string = '';
  @State zHome: string = '';
  @State sysExpanded: boolean = false;
  @State deviceLabel: string = '';
  @State sysVer: string = '';
  @State apiVer: string = '';
  @State screenLabel: string = '';
  @State storageFree: string = '';

  private env: WineEnvService = WineEnvService.getInstance();

  private mirror = (): void => {
    this.perfProfile = this.env.getPerfProfile();
    this.d3dBackend = this.env.getD3dBackend();
    this.zHome = this.env.zHome;
  }

  // 贡献者列表: 名字 / GitHub 链接
  private readonly contributors: Contributor[] = [
    { name: 'hackeris', url: 'https://github.com/hackeris' },
    { name: 'yifengling0', url: 'https://github.com/yifengling0' },
    { name: 'Y1yan', url: 'https://github.com/panedioic' }
  ];
  // 开源仓库 (GitHub): 与贡献者链接同款点击跳转
  private readonly repoUrl: string = 'https://github.com/winehua/WineHua';

  aboutToAppear(): void {
    // 引擎诊断数据镜像 (profile/渲染后端/Z: 映射), 变化经 notify 刷新
    this.env.subscribe(this.mirror);
    this.mirror();
    bundleManager.getBundleInfoForSelf(
      bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION
    ).then((info: bundleManager.BundleInfo) => {
      this.versionName = info.versionName;
    }).catch(() => {
      this.versionName = '';
    });
    // 设备信息 (同步字段): 型号 = 品牌 + 认证型号, 缺品牌时兜底产品型号
    const model = [deviceInfo.brand, deviceInfo.productModel].filter((s: string) => s).join(' ') ||
      deviceInfo.deviceType;
    this.deviceLabel = model;
    this.sysVer = deviceInfo.osFullName;
    this.apiVer = `API ${deviceInfo.sdkApiVersion}`;
    try {
      const d = display.getDefaultDisplaySync();
      this.screenLabel = `${d.width}×${d.height}px ${d.densityPixels}x`;
    } catch (_) {
      this.screenLabel = '';
    }
    // 存储剩余 (异步, 应用数据分区可用空间)
    statfs.getFreeSize('/data/storage/el2/base').then((bytes: number) => {
      this.storageFree = `${(bytes / 1024 / 1024 / 1024).toFixed(1)} GB`;
    }).catch(() => {
      this.storageFree = '获取失败';
    });
  }

  // 点击贡献者行: 经 UIAbilityContext.openLink 拉起系统浏览器。
  // 组件内 getContext(this) 返回基类 Context, 需 cast 才能调 openLink
  private openLink(url: string): void {
    (getContext(this) as common.UIAbilityContext).openLink(url).catch(() => {
      // 无浏览器/拉取失败静默忽略 — 链接文本已展示, 不打断页面
    });
  }

  build() {
    Scroll() {
      Column({ space: 14 }) {
        // ══ 卡 1: 关于万花 (标题与其他块一致; 正文=名称+版本号; 下为功能简介) ══
        Column({ space: 8 }) {
          Text('关于万花')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .width('100%')
          Row() {
            Text('万花（WineHua）')
              .fontSize(14)
              .fontColor($r('app.color.text_primary'))
              .layoutWeight(1)
            Text(this.versionName ? `v${this.versionName}` : '')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
          }
          .width('100%')
          .alignItems(VerticalAlign.Center)
          Text('基于 Wine 与 Box64 指令翻译，让 Windows 程序在 HarmonyOS 设备上运行。内置 Wayland 合成器渲染窗口，支持多窗口、键鼠输入、音频与 OpenGL / DXVK 图形加速。')
            .fontSize(12)
            .fontColor($r('app.color.text_tertiary'))
            .width('100%')
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(12)

        // ══ 卡 2: 开源组件 ══
        Column({ space: 8 }) {
          Text('开源组件')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .width('100%')
          Text('Wine · Box64 · DXVK · VirGL Renderer · Mesa')
            .fontSize(12)
            .fontColor($r('app.color.text_tertiary'))
            .width('100%')
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(12)

        // ══ 卡 3: 开源仓库 ══
        Column({ space: 8 }) {
          Text('开源仓库')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .width('100%')
          Row() {
            Text(this.repoUrl)
              .fontSize(12)
              .fontColor($r('app.color.accent'))
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
          }
          .width('100%')
          .alignItems(VerticalAlign.Center)
          .padding({ top: 4, bottom: 4 })
          .onClick(() => {
            this.openLink(this.repoUrl);
          })
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(12)

        // ══ 卡 4: 贡献者 ══
        Column({ space: 10 }) {
          Text('贡献者')
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .width('100%')
          ForEach(this.contributors, (c: Contributor) => {
            Row({ space: 8 }) {
              Text(c.name)
                .fontSize(14)
                .fontColor($r('app.color.text_primary'))
                .layoutWeight(1)
              Text(c.url)
                .fontSize(11)
                .fontColor($r('app.color.accent'))
                .maxLines(1)
                .textOverflow({ overflow: TextOverflow.Ellipsis })
            }
            .width('100%')
            .alignItems(VerticalAlign.Center)
            .padding({ top: 4, bottom: 4 })
            .onClick(() => {
              this.openLink(c.url);
            })
          }, (c: Contributor) => c.url)
        }
        .width('100%')
        .padding(14)
        .backgroundColor($r('app.color.card_bg'))
        .borderRadius(12)

        // ══ 卡 5: 系统信息 (折叠, 诊断用 — 设备信息 + App/Wine 运行态) ══
        Column() {
          Row() {
            Text('系统信息')
              .fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.text_secondary'))
              .layoutWeight(1)
            SymbolGlyph(this.sysExpanded ? $r('sys.symbol.chevron_down') : $r('sys.symbol.chevron_right'))
              .fontSize(16)
              .fontColor([$r('app.color.text_secondary')])
          }
          .width('100%')
          .padding({ top: 4, bottom: 4 })
          .onClick(() => {
            this.sysExpanded = !this.sysExpanded;
          })
          if (this.sysExpanded) {
            Text(`设备型号: ${this.deviceLabel}`)
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 6 })
            Text(`系统版本: ${this.sysVer}`)
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 4 })
            Text(`API 版本: ${this.apiVer}`)
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 4 })
            Text(`设备类型: ${deviceInfo.deviceType}`)
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 4 })
            Text(`屏幕: ${this.screenLabel}`)
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 4 })
            Text(`存储剩余: ${this.storageFree}`)
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 4 })
            Text(`诊断 profile: ${this.perfProfile}`)
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 4 })
            Text(`渲染后端: ${this.d3dBackend}`)
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 4 })
            Text(`Prefix: ${WINE_PREFIX}`)
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 4 })
            Text(`Z: 映射: ${this.zHome}`)
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              .width('100%').margin({ top: 4 })
          }
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
    // Scroll 通用 align 默认 Alignment.Center — 内容不满屏时会被垂直居中
    // (设置页内容长所以没暴露, 关于页三卡短就露馅了); 显式顶对齐
    .align(Alignment.Top)
    .scrollBar(BarState.Off)
    .backgroundColor($r('app.color.window_bg'))
  }
}

// 贡献者条目: 名字 / GitHub 链接
interface Contributor {
  name: string;
  url: string;
}
