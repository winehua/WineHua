import { SegmentButton, SegmentButtonOptions } from '@kit.ArkUI';
import { compatModeFromIndex } from '../service/Box64Dynarec';

// Windows 版本选项 (下拉): label 为显示名, value 为 wine 侧 version_names 表
// 合法值 (ntdll/version.c 精确 wcscmp, 全小写), 与 winecfg 下拉同款列表
// (win20/30/31 为 DOS 时代, winecfg 也不列, 这里同样不列)。
// 第一项"跟随全局" value 为空 = 不注入。
// ArkTS 对象字面量需显式类型, 故抽 interface
interface WinverOption {
  label: string;
  value: string;
}

const WINVER_OPTIONS: WinverOption[] = [
  // 第 0 项固定为「跟随全局」: winverIdx 初值 0 → 每次 open 默认选中它
  { label: '跟随全局', value: '' },
  // 新 → 旧排列 (最新在前, winecfg 同款浏览习惯)
  { label: 'Windows 11', value: 'win11' },
  { label: 'Windows 10', value: 'win10' },
  { label: 'Windows 8.1', value: 'win81' },
  { label: 'Windows 8', value: 'win8' },
  { label: 'Windows 7', value: 'win7' },
  { label: 'Windows Server 2008 R2', value: 'win2008r2' },
  { label: 'Windows Server 2008', value: 'win2008' },
  { label: 'Windows Vista', value: 'vista' },
  { label: 'Windows Server 2003', value: 'win2003' },
  { label: 'Windows XP 64-Bit', value: 'winxp64' },
  { label: 'Windows XP', value: 'winxp' },
  { label: 'Windows 2000', value: 'win2000' },
  { label: 'Windows NT 4.0', value: 'nt40' },
  { label: 'Windows NT 3.51', value: 'nt351' },
  { label: 'Windows ME', value: 'winme' },
  { label: 'Windows 98', value: 'win98' },
  { label: 'Windows 95', value: 'win95' },
];

// 自定义启动对话框 (应用库/文件页右键菜单「自定义启动」弹出): 临时选择
// 渲染策略 + 兼容模式档位 + Windows 版本, 点「启动」按所选参数启动一次 —
// 不写入持久化配置。
// 每套选择第一项都是「跟随全局」= 不覆盖 (启动时映射为 undefined 传 onLaunch;
// launch 侧回落设置页全局档位, 非出厂基线)。兼容模式为 Box64 动态翻译档位
// (仅 ARM 设备生效, x86_64 原生跑无效果), 对齐 Winlator 4 档位: 稳定/保守/
// 均衡/性能, 每档完整 11 个 BOX64_DYNAREC_* 参数注入 (共享表见
// Box64Dynarec.BOX64_DYNAREC_PRESETS), 覆盖压过全局基线。
// Windows 版本注入 WINEHUA_WINDOWS_VERSION 环境变量, 逐进程生效 (两架构都
// 有效)。版本项多 (18), SegmentButton 上限 5 个按钮放不下, 用 Select 下拉
// (winecfg 同款交互)。
// 每次 open 都是新实例, @State 初值自动回到跟随全局。
@CustomDialog
export struct CustomLaunchDialog {
  controller?: CustomDialogController;
  exeName: string = '';   // 标题 "自定义启动 · <exe>"
  initialArgs: string = ''; // 启动参数输入框初值 (条目持久化的 launchArgs)
  onLaunch: (graphics?: 'dxvk' | 'wined3d',
             compatMode?: 'stability' | 'conservative' | 'intermediate' | 'performance',
             winver?: string,
             launchArgs?: string) => void = () => {};
  // 渲染策略: 0=跟随全局 1=dxvk 2=wined3d
  @State graphicsSel: number[] = [0];
  // 兼容模式: 0=跟随全局 1=稳定 2=保守 3=均衡 4=性能 (对齐 Winlator 4 档位)
  @State compatSel: number[] = [0];
  // 启动参数原文 (TextInput 双向回写; 启动时原文传出, 切分/校验在 service 层)
  @State argsText: string = '';
  // Windows 版本: 按钮显示文本 (选中菜单项后按钮文本自动更新, 回写本状态)。
  // 用 .value() 驱动而不是 .selected(索引): selected 只设"初始选中态/高亮",
  // 不更新按钮显示文本 (收起状态空白); value 才是按钮文本 (HiSH 同款用法)
  @State winverLabel: string = WINVER_OPTIONS[0].label;
  // Select 展示项 (只取 label; 值映射走 WINVER_OPTIONS 按 label 查找)
  private winverOptions: SelectOption[] = WINVER_OPTIONS.map(
    (o: WinverOption): SelectOption => ({ value: o.label }));

  aboutToAppear(): void {
    // 每次 open 都是新实例: @State 初值从这里回到条目持久化参数
    this.argsText = this.initialArgs;
  }

  private doLaunch(): void {
    const g = this.graphicsSel[0] ?? 0;
    const c = this.compatSel[0] ?? 0;
    const opt = WINVER_OPTIONS.find((o: WinverOption) => o.label == this.winverLabel);
    const winver = opt?.value || undefined;
    const args = this.argsText.trim();
    this.onLaunch(g == 1 ? 'dxvk' : g == 2 ? 'wined3d' : undefined,
      compatModeFromIndex(c), winver, args ? args : undefined);
    this.controller?.close();
  }

  build() {
    Column({ space: 16 }) {
      Text(`自定义启动 · ${this.exeName}`)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })

      Column({ space: 8 }) {
        Text('渲染策略')
          .fontSize(13)
          .fontColor($r('app.color.text_secondary'))
        SegmentButton({
          options: SegmentButtonOptions.capsule({
            buttons: [{ text: '跟随全局' }, { text: 'DXVK' }, { text: 'WineD3D' }],
            multiply: false
          }),
          selectedIndexes: $graphicsSel,
          onItemClicked: (index: number) => {
            this.graphicsSel = [index];
          }
        })
      }
      .width('100%')
      .alignItems(HorizontalAlign.Start)

      Column({ space: 8 }) {
        Text('兼容模式')
          .fontSize(13)
          .fontColor($r('app.color.text_secondary'))
        SegmentButton({
          options: SegmentButtonOptions.capsule({
            buttons: [
              { text: '跟随全局' }, { text: '稳定' }, { text: '保守' },
              { text: '均衡' }, { text: '性能' }
            ],
            multiply: false
          }),
          selectedIndexes: $compatSel,
          onItemClicked: (index: number) => {
            this.compatSel = [index];
          }
        })
        Text('跟随全局 = 使用设置页「兼容模式」档位')
          .fontSize(11)
          .fontColor($r('app.color.text_tertiary'))
      }
      .width('100%')
      .alignItems(HorizontalAlign.Start)

      Column({ space: 8 }) {
        Text('Windows 版本')
          .fontSize(13)
          .fontColor($r('app.color.text_secondary'))
        Select(this.winverOptions)
          .value(this.winverLabel)
          .width('100%')
          .onSelect((index: number) => {
            this.winverLabel = WINVER_OPTIONS[index].label;
          })
        Text('注入 WINEHUA_WINDOWS_VERSION, 仅本次启动生效')
          .fontSize(11)
          .fontColor($r('app.color.text_tertiary'))
      }
      .width('100%')
      .alignItems(HorizontalAlign.Start)

      Column({ space: 8 }) {
        Text('启动参数')
          .fontSize(13)
          .fontColor($r('app.color.text_secondary'))
        TextInput({ text: this.argsText, placeholder: '如 -noreactlogin (空格分隔多个)' })
          .width('100%')
          .height(40)
          .onChange((value: string) => {
            this.argsText = value;
          })
        Text('启动成功后自动记住, 之后普通启动也带上; 清空即清除')
          .fontSize(11)
          .fontColor($r('app.color.text_tertiary'))
      }
      .width('100%')
      .alignItems(HorizontalAlign.Start)

      Row({ space: 12 }) {
        Button('取消')
          .layoutWeight(1)
          .backgroundColor($r('app.color.control_bg'))
          .onClick(() => this.controller?.close())
        Button('启动')
          .layoutWeight(1)
          .onClick(() => {
            this.doLaunch();
          })
      }
      .width('100%')
    }
    .padding(18)
    .backgroundColor($r('app.color.overlay_bg'))
    .borderRadius(12)
  }
}
