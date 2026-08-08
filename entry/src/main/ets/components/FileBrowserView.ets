import { hilog } from '@kit.PerformanceAnalysisKit';
import { fileIo as fs, fileUri, picker } from '@kit.CoreFileKit';
import { promptAction } from '@kit.ArkUI';
import { common, Want } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { WineEnvService, DRIVE_C } from '../service/WineEnvService';
import { AppLibraryService, AppEntry } from '../service/AppLibraryService';
import { IconExtractor } from '../service/IconExtractor';
import { EmptyState } from './EmptyState';

const DOMAIN = 0x0000;
const TAG = 'Files';

// 文件页 (设计 4.4 / P3): 双根节点 C:\(prefix drive_c) 与 Z:\(zHome 下载目录)。
// 顶层为根选择视图, 进入某根后 "← 上级目录" 可一路退回根选择。
// exe 行尾只留"启动"小按钮, 其余操作(加入应用库/重命名/复制/剪切/删除)收进
// 长按(PC 右键)菜单; 剪切跨盘(C: 沙箱 ↔ Z: 共享存储)renameSync 会失败,
// 回退为递归复制+删除。导入 = 系统文件选择器把 exe/lnk 拷入 Z:\ 根,
// exe 同时自动入库 (设计原文"触发重新扫描"的可感知效果就是应用库出现新条目)。
// 列表条目: 一次 statSync 全量取回 (含大小/修改时间), 避免排序与渲染重复 stat。
// icon 为 IconExtractor 提取的 PNG 缓存路径, '' = 未提取 (UI 回退系统符号)
interface FileEntry {
  name: string;
  isDir: boolean;
  size: number;     // 字节; 目录为 -1 (大小列仅文件显示)
  mtime: number;    // 毫秒时间戳
  icon: string;     // PNG 缓存路径或 ''
}

interface DriveRoot {
  key: string;      // 'C' | 'Z'
  label: string;    // 显示名 "C:\" / "Z:\"
  sub: string;      // 副标题说明
  path: string;     // native 根路径 (无结尾斜杠)
}

// 页内剪贴板 (单条目): cut=true 粘贴后清空, copy 可反复粘贴
interface ClipItem {
  path: string;     // 完整 native 路径
  cut: boolean;
}

// 重命名输入对话框 (与应用库 RenameDialog 同款模式, 文件名校验在 onDone 侧)
@CustomDialog
struct FileRenameDialog {
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

@Component
export struct FileBrowserView {
  // -- 镜像自 service 的状态 (订阅驱动, 见 aboutToAppear) --
  @State state: string = 'idle';
  @State prefixReady: boolean = false;
  @State libEntries: AppEntry[] = [];
  // -- 页面自有状态 --
  @State curDir: string = DRIVE_C;
  @State entries: FileEntry[] = [];
  @State dirStack: string[] = [];
  // true = 根选择视图 (C:\ / Z:\ 两个节点); false = 在某根内浏览
  @State atRootList: boolean = true;
  // 页内剪贴板 (@State 驱动标题栏"粘贴"按钮显隐)
  @State clipItem: ClipItem | null = null;
  // 外部定位目录请求 (Index 注入, 应用库"打开所在目录"联动); 消费后自清空
  @Link @Watch('onFilesDirRequest') filesDirRequest: string;

  private env: WineEnvService = WineEnvService.getInstance();
  private lib: AppLibraryService = AppLibraryService.getInstance();
  // 上一次的 prefixReady 镜像值, 用于边沿检测 (避免每次 notify 都重复 listDir)
  private prevPrefixReady: boolean = false;
  private renameController: CustomDialogController | null = null;

  private mirrorEnv = (): void => {
    this.state = this.env.engineState;
    this.prefixReady = this.env.prefixReady;

    // prefix 就绪/失效时的目录联动 (边沿检测后只触发一次)
    if (this.prefixReady != this.prevPrefixReady) {
      if (this.prefixReady) {
        this.backToRootList();
      } else {
        this.entries = [];
      }
      this.prevPrefixReady = this.prefixReady;
    }
  }

  private mirrorLib = (): void => {
    this.libEntries = this.lib.entries;
  }

  aboutToAppear(): void {
    this.env.subscribe(this.mirrorEnv);
    this.lib.subscribe(this.mirrorLib);
    this.mirrorEnv();
    this.mirrorLib();
  }

  // Z: 根 = env.zHome (初始化时 picker 取回, 可能带结尾斜杠, 统一去掉)
  private zRoot(): string {
    const z = this.env.zHome;
    return z.endsWith('/') ? z.substring(0, z.length - 1) : z;
  }

  private roots(): DriveRoot[] {
    return [
      { key: 'C', label: 'C:\\', sub: '系统目录 (Wine 容器)', path: DRIVE_C },
      { key: 'Z', label: 'Z:\\', sub: '下载目录 (与系统共享)', path: this.zRoot() }
    ];
  }

  // path 所属的根 (前缀匹配; 都不属于返回 null — 理论上不应发生)
  private rootOf(path: string): DriveRoot | null {
    for (const r of this.roots()) {
      if (path == r.path || path.startsWith(r.path + '/')) {
        return r;
      }
    }
    return null;
  }

  private listDir = (path: string): void => {
    try {
      let names = fs.listFileSync(path);
      // 一次 stat 全量: 大小/修改时间/目录判定一起取, 不再为排序逐项重复 stat
      const list: FileEntry[] = [];
      for (const n of names) {
        try {
          const st = fs.statSync(path + '/' + n);
          list.push({
            name: n,
            isDir: st.isDirectory(),
            size: st.isDirectory() ? -1 : Number(st.size),
            // statSync 的 mtime 单位是秒 (非毫秒), 直接喂给 Date 会显示成
            // 1970-01-21 之类的假日期 — 这里统一乘 1000 转成毫秒时间戳
            mtime: Number(st.mtime) * 1000,
            icon: ''
          });
        } catch (_) {
          list.push({ name: n, isDir: false, size: -1, mtime: 0, icon: '' });
        }
      }
      list.sort((a, b) => {
        if (a.isDir && !b.isDir) return -1;
        if (!a.isDir && b.isDir) return 1;
        if (!a.isDir && !b.isDir) {
          let aExe = this.isExe(a.name) ? 0 : 1;
          let bExe = this.isExe(b.name) ? 0 : 1;
          return aExe - bExe || a.name.localeCompare(b.name);
        }
        return a.name.localeCompare(b.name);
      });
      this.entries = list;
      this.loadIcons();
    } catch (e) {
      // 曾静默吞错 — Z: 路径未就绪/权限异常时页面空白无任何线索
      hilog.error(DOMAIN, TAG, 'listDir fail: %{public}s err=%{public}s', path, JSON.stringify(e));
      this.entries = [];
    }
  }

  // ---- 列格式化 ----

  // 文件大小: B/KB/MB/GB/TB; 目录 (size<0) 显示 — (仅针对文件)
  private fmtSize(size: number): string {
    if (size < 0) {
      return '—';
    }
    if (size < 1024) {
      return `${size} B`;
    }
    const units: string[] = ['KB', 'MB', 'GB', 'TB'];
    let v = size;
    let u = -1;
    while (v >= 1024 && u < units.length - 1) {
      v /= 1024;
      u++;
    }
    return `${v >= 100 ? v.toFixed(0) : v.toFixed(1)} ${units[u]}`;
  }

  // 修改时间: YYYY-MM-DD HH:mm (本地时间); mtime 无效 (<=0) 时留空
  private fmtTime(ms: number): string {
    if (ms <= 0) {
      return '';
    }
    const d = new Date(ms);
    const p = (v: number): string => (v < 10 ? '0' : '') + v;
    return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`;
  }

  // ---- exe 图标 (复用应用库的 IconExtractor: PE 解析 + PNG 缓存) ----

  // 提取失败记忆 (非 @State, 只防重试不触发渲染)
  private iconFail: Set<string> = new Set();

  // 对当前目录的 exe 异步提取图标: 每批 4 个, 批间让出一帧, 目录 exe 多时
  // UI 不卡; IconExtractor 内部按路径哈希磁盘缓存, 重复进目录秒回
  private async loadIcons(): Promise<void> {
    const paths: string[] = [];
    for (const e of this.entries) {
      if (!e.isDir && this.isExe(e.name) && !e.icon && !this.iconFail.has(this.fullPath(e.name))) {
        paths.push(this.fullPath(e.name));
      }
    }
    const dir = this.curDir;
    for (let i = 0; i < paths.length; i += 4) {
      const batch = paths.slice(i, i + 4);
      await Promise.all(batch.map(async (p: string) => {
        const icon = await IconExtractor.getInstance().extract(p);
        // 提取期间用户可能已切换目录: 只更新当前目录的对应行, 防错配
        if (this.curDir != dir) {
          return;
        }
        if (!icon) {
          this.iconFail.add(p);
          return;
        }
        const name = p.substring(p.lastIndexOf('/') + 1);
        this.entries = this.entries.map((e: FileEntry) =>
          e.name == name && !e.icon ?
            { name: e.name, isDir: e.isDir, size: e.size, mtime: e.mtime, icon: icon } : e);
      }));
    }
  }

  private isDir = (path: string): boolean => {
    try { return fs.statSync(path).isDirectory(); } catch (_) { return false; }
  }

  private pathExists = (path: string): boolean => {
    try { return fs.accessSync(path); } catch (_) { return false; }
  }

  // 外部定位: 接受 C: / Z: 任一根内的目录; 存在且为目录则重建 dirStack 并
  // listDir, 保证"← 上级目录"可一路退回根选择视图
  private onFilesDirRequest(): void {
    const target: string = this.filesDirRequest;
    if (target.length == 0) {
      return;
    }
    this.filesDirRequest = '';
    const root = this.rootOf(target);
    if (!root || !this.pathExists(target) || !this.isDir(target)) {
      return;
    }
    this.dirStack = [];
    if (target != root.path) {
      let p: string = root.path;
      const segs: string[] = target.substring(root.path.length + 1).split('/');
      for (let i = 0; i < segs.length - 1; i++) {
        this.dirStack.push(p);
        p = p + '/' + segs[i];
      }
      this.dirStack.push(p);
    }
    this.curDir = target;
    this.atRootList = false;
    this.listDir(target);
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

  private enterRoot(root: DriveRoot): void {
    // 路径在点击时重新解析, 不用渲染期快照 root.path: 桌面会话已存活时
    // prefixReady 经 waitForPrefix 秒置真, 而 zHome 要等 doInit 的 picker
    // 异步返回才赋值 — 窗口期内快照的 Z: path 是 '', 进入即空目录且
    // rootOf 失配 (路径栏空白、无"系统文件管理"按钮), 只能返回重进
    const path = root.key == 'Z' ? this.zRoot() : root.path;
    if (!path) {
      this.toast('Z: 目录尚未就绪，请稍候重试');
      return;
    }
    this.dirStack = [];
    this.curDir = path;
    this.atRootList = false;
    this.listDir(path);
  }

  private backToRootList(): void {
    this.atRootList = true;
    this.dirStack = [];
    this.entries = [];
  }

  private goUp = (): void => {
    let parent = this.dirStack.pop();
    if (parent) {
      this.curDir = parent;
      this.listDir(parent);
    } else {
      // 已在某根的顶层: 再上一级 = 根选择视图
      this.backToRootList();
    }
  }

  private displayPath = (): string => {
    if (this.atRootList) {
      return '此设备';
    }
    const root = this.rootOf(this.curDir);
    if (!root) {
      return this.curDir;
    }
    if (this.curDir == root.path) {
      return root.label;
    }
    return root.label + this.curDir.substring(root.path.length + 1).replace(/\//g, '\\');
  }

  private inLibrary = (path: string): boolean => {
    for (const entry of this.libEntries) {
      if (entry.path == path) {
        return true;
      }
    }
    return false;
  }

  // 行类别: 基于 listDir 已取回的 isDir, 不再重复 stat
  private rowKindOf = (entry: FileEntry): 'dir' | 'exe' | 'other' => {
    return entry.isDir ? 'dir' : (this.isExe(entry.name) ? 'exe' : 'other');
  }

  private toast(msg: string): void {
    promptAction.showToast({ message: msg, duration: 2000 });
  }

  // ---- 文件操作 ----

  private fullPath(name: string): string {
    return this.curDir + '/' + name;
  }

  // 递归复制 (目录逐层展开; 跨盘剪切回退也走它)
  private copyRecursive(src: string, dst: string): void {
    if (this.isDir(src)) {
      fs.mkdirSync(dst);
      const names = fs.listFileSync(src);
      for (const n of names) {
        this.copyRecursive(src + '/' + n, dst + '/' + n);
      }
    } else {
      fs.copyFileSync(src, dst);
    }
  }

  private deleteRecursive(path: string): void {
    if (this.isDir(path)) {
      const names = fs.listFileSync(path);
      for (const n of names) {
        this.deleteRecursive(path + '/' + n);
      }
      fs.rmdirSync(path);
    } else {
      fs.unlinkSync(path);
    }
  }

  private openRename(name: string): void {
    this.renameController = new CustomDialogController({
      builder: FileRenameDialog({
        initial: name,
        onDone: (newName: string) => {
          this.doRename(name, newName);
        }
      }),
      customStyle: true
    });
    this.renameController.open();
  }

  private doRename(name: string, newName: string): void {
    const trimmed = newName.trim();
    if (!trimmed || trimmed == name) {
      return;
    }
    if (trimmed.indexOf('/') >= 0) {
      this.toast('名称不能包含 /');
      return;
    }
    const dst = this.fullPath(trimmed);
    if (this.pathExists(dst)) {
      this.toast('同名文件已存在');
      return;
    }
    try {
      fs.renameSync(this.fullPath(name), dst);
      this.listDir(this.curDir);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'rename fail: %{public}s', JSON.stringify(e));
      this.toast('重命名失败');
    }
  }

  private confirmDelete(entry: FileEntry): void {
    const kind = this.rowKindOf(entry);
    AlertDialog.show({
      title: '删除' + (kind == 'dir' ? '目录' : '文件'),
      message: `将删除 ${entry.name}` + (kind == 'dir' ? ' 及其全部内容' : '') + '，且不可恢复。确定继续？',
      primaryButton: {
        value: '取消',
        action: () => {}
      },
      secondaryButton: {
        value: '删除',
        fontColor: $r('app.color.danger'),
        action: () => {
          try {
            this.deleteRecursive(this.fullPath(entry.name));
            this.listDir(this.curDir);
          } catch (e) {
            hilog.error(DOMAIN, TAG, 'delete fail: %{public}s', JSON.stringify(e));
            this.toast('删除失败');
          }
        }
      }
    });
  }

  private setClip(name: string, cut: boolean): void {
    this.clipItem = { path: this.fullPath(name), cut: cut };
    this.toast(cut ? '已剪切，到目标目录点粘贴' : '已复制，到目标目录点粘贴');
  }

  // 粘贴到当前目录: 复制直接递归拷; 剪切先试 renameSync (同盘零拷贝),
  // 跨盘 (EXDEV 等) 回退递归复制+删除。目标已存在则中止不覆盖 (保守)
  private pasteClip(): void {
    const item = this.clipItem;
    if (!item || this.atRootList) {
      return;
    }
    const name = item.path.substring(item.path.lastIndexOf('/') + 1);
    const dst = this.curDir + '/' + name;
    if (dst == item.path) {
      this.toast('源与目标相同');
      return;
    }
    if (this.pathExists(dst)) {
      this.toast('目标位置已存在同名项');
      return;
    }
    try {
      if (item.cut) {
        try {
          fs.renameSync(item.path, dst);
        } catch (_) {
          this.copyRecursive(item.path, dst);
          this.deleteRecursive(item.path);
        }
        this.clipItem = null;
      } else {
        this.copyRecursive(item.path, dst);
      }
      this.listDir(this.curDir);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'paste fail: %{public}s', JSON.stringify(e));
      this.toast('粘贴失败');
    }
  }

  // fd 静默关闭 (导入复制中途抛异常时 finally 兜底, 防 fd 泄漏)
  private closeQuietly(f: fs.File | null): void {
    if (!f) {
      return;
    }
    try {
      fs.closeSync(f);
    } catch (_) {}
  }
  // 系统文件管理器打开目录 (参考 HiSH SharedFolderContent.openInFileManager):
  // 显式拉起 com.huawei.hmos.filemanager/MainAbility, fileUri 参数定位目录。
  // 只用于 Z: (共享存储); C: 是应用沙箱, 系统文件管理器无权也不该展示
  private openInFileManager(path: string): void {
    try {
      const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const want: Want = {
        bundleName: 'com.huawei.hmos.filemanager',
        abilityName: 'MainAbility',
        parameters: {
          'fileUri': fileUri.getUriFromPath(path)
        }
      };
      ctx.startAbility(want).catch((e: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'open file manager fail: %{public}s', JSON.stringify(e));
        this.toast('打开系统文件管理失败');
      });
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'open file manager fail: %{public}s', JSON.stringify(e));
      this.toast('打开系统文件管理失败');
    }
  }

  // 导入 (设计 4.4): 系统文件选择器选 exe/lnk → 拷入 Z:\ 根; exe 自动入库
  // (后缀不在 picker 侧过滤 — 依赖系统对冷门后缀的筛选有翻车风险, 改为
  // 选中后校验, 不合格逐项跳过并提示)
  private importFiles(): void {
    const zRoot = this.zRoot();
    if (!zRoot) {
      this.toast('Z: 目录不可用');
      return;
    }
    const docPicker = new picker.DocumentViewPicker(getContext(this));
    docPicker.select({ maxSelectNumber: 9 }).then((uris: string[]) => {
      if (!uris || uris.length == 0) {
        return;
      }
      let copied = 0;
      let skipped = 0;
      for (const uri of uris) {
        let name = '';
        try {
          name = new fileUri.FileUri(uri).name;
        } catch (_) {
          skipped++;
          continue;
        }
        const lower = name.toLowerCase();
        if (!lower.endsWith('.exe') && !lower.endsWith('.lnk')) {
          skipped++;
          continue;
        }
        const dst = zRoot + '/' + name;
        if (this.pathExists(dst)) {
          skipped++;
          continue;
        }
        let srcFile: fs.File | null = null;
        let dstFile: fs.File | null = null;
        try {
          // picker 授权的是 URI, openSync 直接吃 URI; 目标走沙箱路径
          srcFile = fs.openSync(uri, fs.OpenMode.READ_ONLY);
          dstFile = fs.openSync(dst,
            fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
          fs.copyFileSync(srcFile.fd, dstFile.fd);
          copied++;
          if (lower.endsWith('.exe')) {
            this.lib.add(dst);
          }
        } catch (e) {
          hilog.error(DOMAIN, TAG, 'import copy fail: %{public}s', JSON.stringify(e));
          skipped++;
        } finally {
          this.closeQuietly(srcFile);
          this.closeQuietly(dstFile);
        }
      }
      this.toast(`已导入 ${copied} 个文件` + (skipped > 0 ? `，跳过 ${skipped} 个` : ''));
      if (!this.atRootList && this.curDir == zRoot) {
        this.listDir(this.curDir);
      }
    }).catch((e: Object) => {
      hilog.error(DOMAIN, TAG, 'picker fail: %{public}s', JSON.stringify(e));
    });
  }

  // exe 启动统一入口 (行内按钮与长按菜单同判据): 引擎 ready 才放行,
  // 受理失败给 toast; 受理后的闪退检测在应用库启动反馈里 (文件页无加载态)
  private async launchExe(path: string): Promise<void> {
    if (this.env.engineState != 'ready') {
      return;
    }
    // 已就绪但无桌面会话：先拉起桌面（与应用库启动同规则），避免程序只有自己窗口没有桌面
    if (!this.env.sessionAlive) {
      const ok = await this.env.ensureDesktop();
      if (!ok) {
        this.toast('桌面启动失败，可到环境页重启引擎后重试');
        return;
      }
    }
    const pid = this.lib.launch(path);
    if (pid <= 0) {
      this.toast('启动失败，可到环境页切换渲染策略后重试');
    }
  }

  // ---- 菜单与行 ----

  // 长按(PC 右键)菜单: exe 多 启动/加入应用库 两项, 文件操作全行可用
  @Builder
  fileMenu(entry: FileEntry) {
    Menu() {
      if (this.rowKindOf(entry) == 'exe') {
        MenuItem({ content: '启动' })
          .onChange(() => {
            this.launchExe(this.fullPath(entry.name));
          })
        if (!this.inLibrary(this.fullPath(entry.name))) {
          MenuItem({ content: '加入应用库' })
            .onChange(() => {
              this.lib.add(this.fullPath(entry.name));
            })
        }
      }
      MenuItem({ content: '重命名' })
        .onChange(() => {
          this.openRename(entry.name);
        })
      MenuItem({ content: '复制' })
        .onChange(() => {
          this.setClip(entry.name, false);
        })
      MenuItem({ content: '剪切' })
        .onChange(() => {
          this.setClip(entry.name, true);
        })
      MenuItem({ content: '删除' })
        .onChange(() => {
          this.confirmDelete(entry);
        })
    }
  }

  // 文件行内容 (exe 与 dir/other 共用; bindContextMenu 不接受 undefined,
  // 故菜单按行类别在 ListItem 上条件挂载, 行体不变)。
  // 列布局: 图标 | 名称(自适应) | 大小(仅文件) | 修改时间 | 操作
  @Builder
  fileRow(entry: FileEntry) {
    Row() {
      // 行首图标: exe 有提取图标用提取的 PNG, 否则系统符号占位
      if (this.rowKindOf(entry) == 'exe' && entry.icon) {
        Image(`file://${entry.icon}`)
          .width(26).height(26)
          .margin({ right: 2 })
      } else {
        SymbolGlyph(this.rowKindOf(entry) == 'dir' ? $r('sys.symbol.folder_fill') :
                    this.rowKindOf(entry) == 'exe' ? $r('sys.symbol.play_fill') : $r('sys.symbol.doc'))
          .fontSize(20)
          .fontColor([this.rowKindOf(entry) == 'dir' ? $r('app.color.state_info') :
                      this.rowKindOf(entry) == 'exe' ? $r('app.color.accent') : $r('app.color.text_tertiary')])
          .width(28)
      }
      Text(entry.name)
        .fontColor(this.rowKindOf(entry) == 'exe' ? $r('app.color.text_primary') :
                   this.rowKindOf(entry) == 'dir' ? $r('app.color.state_info') : $r('app.color.text_tertiary'))
        .layoutWeight(1)
        .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
      // 大小列 (仅文件; 目录显示 —)
      Text(this.rowKindOf(entry) == 'dir' ? '—' : this.fmtSize(entry.size))
        .width(72).fontSize(11)
        .fontColor($r('app.color.text_tertiary'))
        .textAlign(TextAlign.End).maxLines(1)
      // 修改时间列
      Text(this.fmtTime(entry.mtime))
        .width(126).fontSize(11)
        .fontColor($r('app.color.text_tertiary'))
        .textAlign(TextAlign.End).maxLines(1)
      // 行尾操作槽: 固定宽度 + 右对齐, 非 exe 行也保留同样宽度 —
      // 否则无"启动"按钮的行, 大小/时间列相对 exe 行整体右移错位
      Row() {
        if (this.rowKindOf(entry) == 'exe') {
          Button('启动')
            .height(32)
            .enabled(this.state == 'ready')
            .onClick(() => this.launchExe(this.fullPath(entry.name)))
        } else if (this.rowKindOf(entry) == 'dir') {
          SymbolGlyph($r('sys.symbol.chevron_right'))
            .fontSize(16)
            .fontColor([$r('app.color.text_tertiary')])
        }
      }
      .width(88)
      .margin({ left: 14 })
      .justifyContent(FlexAlign.End)
      .alignItems(VerticalAlign.Center)
    }
    .width('100%').height(44).padding({ left: 14, right: 14 })
    .alignItems(VerticalAlign.Center)
  }

  // 根选择视图: C:\ / Z:\ 两个节点 (设计 4.4 "根节点两个")
  @Builder
  rootRow(root: DriveRoot) {
    Row() {
      SymbolGlyph(root.key == 'C' ? $r('sys.symbol.archivebox') : $r('sys.symbol.folder_fill'))
        .fontSize(22)
        .fontColor([$r('app.color.accent')])
        .width(30)
      Column({ space: 2 }) {
        Text(root.label)
          .fontColor($r('app.color.text_primary'))
        Text(root.sub)
          .fontSize(11)
          .fontColor($r('app.color.text_tertiary'))
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)
      .margin({ left: 6 })
      SymbolGlyph($r('sys.symbol.chevron_right'))
        .fontSize(16)
        .fontColor([$r('app.color.text_tertiary')])
    }
    .width('100%').height(56).padding({ left: 14, right: 14 })
    .alignItems(VerticalAlign.Center)
    .backgroundColor($r('app.color.card_bg'))
    .onClick(() => {
      this.enterRoot(root);
    })
  }

  build() {
    Column() {
      if (this.prefixReady) {
        Row() {
          if (!this.atRootList) {
            // 与右侧"粘贴/系统文件管理/导入"统一 32 高度, 避免默认 40 显得突兀
            Button('← 上级目录')
              .height(32)
              .backgroundColor($r('app.color.control_bg'))
              .onClick(this.goUp)
          }
          Text(this.displayPath())
            .fontColor($r('app.color.text_tertiary'))
            .margin({ left: 10 }).layoutWeight(1)
            .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
          if (this.clipItem && !this.atRootList) {
            Button('粘贴')
              .height(32)
              .backgroundColor($r('app.color.control_bg'))
              .onClick(() => {
                this.pasteClip();
              })
          }
          // 浏览 Z: 内部时: 标题栏给"系统文件管理"入口 (打开当前目录)
          if (!this.atRootList && this.rootOf(this.curDir)?.key == 'Z') {
            Button('系统文件管理')
              .height(32)
              .fontSize(12)
              .margin({ left: 8 })
              .backgroundColor($r('app.color.control_bg'))
              .onClick(() => {
                this.openInFileManager(this.curDir);
              })
          }
          // "导入"只在进入目录后出现 — 首页(根选择)顶行保持干净, 仅留路径文本
          if (!this.atRootList) {
            Button('导入')
              .height(32)
              .margin({ left: 8 })
              .backgroundColor($r('app.color.accent'))
              .onClick(() => {
                this.importFiles();
              })
          }
        }
        .width('100%').padding({ left: 14, right: 14, top: 10, bottom: 6 })

        if (this.atRootList) {
          List({ space: 1 }) {
            ForEach(this.roots(), (root: DriveRoot) => {
              ListItem() {
                this.rootRow(root)
              }
            }, (root: DriveRoot) => root.key)
          }
          .width('100%').layoutWeight(1)
        } else {
          List({ space: 1 }) {
            ForEach(this.entries, (entry: FileEntry) => {
              if (this.rowKindOf(entry) == 'exe') {
                ListItem() {
                  this.fileRow(entry)
                }
                .backgroundColor($r('app.color.card_bg'))
                .hoverEffect(HoverEffect.Highlight)
                // modalMode: 默认 AUTO 当前等同 NONE — 菜单外点击穿透给下层
                // (其它行/目录会同时响应). TARGET_WINDOW 使应用窗口不接收菜单外
                // 点击, 点空白仅关闭菜单, 与系统文件管理器行为一致
                .bindContextMenu(this.fileMenu(entry), ResponseType.LongPress,
                  { modalMode: ModalMode.TARGET_WINDOW })
              } else {
                ListItem() {
                  this.fileRow(entry)
                }
                .backgroundColor($r('app.color.card_bg'))
                .hoverEffect(HoverEffect.Highlight)
                // modalMode 同上方 exe 分支: 菜单外点击仅关菜单, 不穿透下层
                .bindContextMenu(this.fileMenu(entry), ResponseType.LongPress,
                  { modalMode: ModalMode.TARGET_WINDOW })
                .onClick(() => {
                  if (entry.isDir) {
                    this.enterDir(entry.name);
                  }
                })
              }
            }, (entry: FileEntry) => entry.name)
          }
          .width('100%').layoutWeight(1)
        }
      } else {
        Column() {
          EmptyState({
            icon: $r('sys.symbol.archivebox'),
            title: '需要先初始化 Wine 环境'
          })
        }
        .width('100%').layoutWeight(1).justifyContent(FlexAlign.Center)
      }
    }
    .width('100%').height('100%')
    .backgroundColor($r('app.color.window_bg'))
  }
}
