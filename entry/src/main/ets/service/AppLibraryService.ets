import { hilog } from '@kit.PerformanceAnalysisKit';
import { common } from '@kit.AbilityKit';
import { preferences } from '@kit.ArkData';
import { fileIo as fs } from '@kit.CoreFileKit';
import testNapi from 'libentry.so';
import { WineEnvService } from './WineEnvService';
import { IconExtractor } from './IconExtractor';
import { LnkScanner, LnkTarget } from './LnkScanner';

const DOMAIN = 0x0000;
const STORE_NAME = 'app_library';
const KEY_ENTRIES = 'entries';
const WINE_PREFIX = '/data/storage/el2/base/files/.wine';

// 单应用配置 (P2): 只收能翻译为用户语言的组合 — 图形渲染 / 兼容模式 / 启动参数。
// 字段全可选, 缺省 = 跟随全局或默认行为 (继承: 全局 → 应用两级)。
// 刻意不暴露裸环境变量 / WINEDEBUG / presentBackend 等底层开关
// (无法翻译成用户可理解的语义, 用户决策: 不好翻译的暂不支持)
export interface AppConfig {
  graphics?: string;  // 'dxvk'(硬件加速) | 'wined3d'(兼容渲染); 缺省 = 跟随全局 d3dBackend
  compat?: boolean;   // 兼容模式: BOX64 保守 dynarec 组合 (应用闪退/异常时开启, 速度变慢)
  args?: string;      // 启动参数, 空格分隔 (如 "-windowed")
}

export interface AppEntry {
  path: string;
  name: string;
  icon?: string; // 图标 PNG 缓存路径 (IconExtractor 异步提取, 完成前为空, UI 回退 ▶)
  favorite?: boolean;  // 收藏 (筛选 chips, 条目上显示"收藏"标签)
  builtin?: boolean;   // 内建工具 (wine 自带, 标签"内建"; 不可移除/隐藏)
  hidden?: boolean;    // 用户隐藏 (各 chips 均不显示, "已隐藏"chip 可见)
  config?: AppConfig;  // 单应用配置 (缺省 = 全部跟随全局)
}

// withField 的可选覆盖项 (ArkTS 不用 Partial 以保持显式)
interface AppEntryPatch {
  path?: string;
  name?: string;
  icon?: string;
  favorite?: boolean;
  builtin?: boolean;
  hidden?: boolean;
  config?: AppConfig;
}

// 内建工具条目描述 (ArkTS 要求显式类型, 不用匿名对象字面量)
interface BuiltinTool {
  name: string;
  exe: string;
}

// 内建工具 (设计 4.1): wine 自带程序, prefix 就绪后确保入库。
// exe 为相对 drive_c 的路径。注意 regedit 是特例: wine 的 [FakeDlls]
// (loader/wine.inf.in) 只把它装进 C:\windows, 不装 system32。
// 路径为 native 形式 (与文件页入库/lnk 扫描一致), 仅在文件存在时加入。
const BUILTIN_TOOLS: BuiltinTool[] = [
  { name: '文件管理器', exe: 'windows/system32/explorer.exe' },
  { name: '记事本', exe: 'windows/system32/notepad.exe' },
  { name: 'Wine 配置', exe: 'windows/system32/winecfg.exe' },
  { name: '注册表编辑器', exe: 'windows/regedit.exe' },
  { name: '任务管理器', exe: 'windows/system32/taskmgr.exe' },
  { name: '命令提示符', exe: 'windows/system32/cmd.exe' },
  { name: '控制面板', exe: 'windows/system32/control.exe' },
];

// 应用库服务: 收藏的 exe 列表, preferences 持久化 (store 'app_library', 单 key 'entries' 存 JSON)。
// 页面经 subscribe 镜像 entries; launch 与原 Index.doRun 一致 (统一走 WINE_BIN)。
// 注意: entries 每次变更都换新数组引用, 保证页面 @State 镜像能触发刷新。
export class AppLibraryService {
  private static instance: AppLibraryService | null = null;

  static getInstance(): AppLibraryService {
    if (!AppLibraryService.instance) {
      AppLibraryService.instance = new AppLibraryService();
    }
    return AppLibraryService.instance;
  }

  entries: AppEntry[] = [];

  private store: preferences.Preferences | null = null;
  private initialized: boolean = false;
  private listeners: Array<() => void> = [];

  // 极简订阅: 忽略重复引用 (组件把 listener 存成员字段重复订阅时防累积)
  subscribe(listener: () => void): void {
    if (this.listeners.indexOf(listener) >= 0) {
      return;
    }
    this.listeners.push(listener);
  }

  private notify(): void {
    for (const listener of this.listeners) {
      listener();
    }
  }

  // 幂等: 加载持久化的条目并按 name 排序
  init(ctx: common.Context): void {
    if (this.initialized) {
      return;
    }
    this.initialized = true;
    try {
      this.store = preferences.getPreferencesSync(ctx, { name: STORE_NAME });
      const json = this.store.getSync(KEY_ENTRIES, '[]') as string;
      const list = JSON.parse(json) as AppEntry[];
      if (Array.isArray(list)) {
        this.entries = list;
        this.sortEntries();
      }
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'app library load fail: %{public}s', JSON.stringify(e));
      this.entries = [];
    }
    // 存量条目补图标 (老数据无 icon 字段, 首次启动后台逐个提取)
    for (const e of this.entries) {
      if (!e.icon) {
        this.fillIcon(e.path);
      }
    }
  }

  // name 取文件名去 .exe 后缀; 按 path 去重, 已存在则忽略
  add(path: string): void {
    const fileName = path.substring(path.lastIndexOf('/') + 1);
    const name = fileName.toLowerCase().endsWith('.exe') ? fileName.substring(0, fileName.length - 4) : fileName;
    this.addNamed(path, name);
  }

  addNamed(path: string, name: string): void {
    if (this.has(path)) {
      return;
    }
    this.entries = this.entries.concat([{ path: path, name: name }]);
    this.sortEntries();
    this.persist();
    this.notify();
    this.fillIcon(path);
  }

  // 扫描 Start Menu / Desktop 的 .lnk 入库 (M3, 手动触发), 返回新增数量
  scanShortcuts(): number {
    const found: LnkTarget[] = LnkScanner.getInstance().scan(WINE_PREFIX, WineEnvService.getInstance().zHome);
    let added = 0;
    for (const t of found) {
      if (!this.has(t.target)) {
        this.addNamed(t.target, t.name);
        added++;
      }
    }
    return added;
  }

  // 复制 entry 并覆盖指定字段 (可选字段 undefined 保留原值) —
  // entries 元素整体替换, 保证页面 @State 镜像刷新
  private withField(e: AppEntry, patch: AppEntryPatch): AppEntry {
    return {
      path: e.path,
      name: patch.name !== undefined ? patch.name : e.name,
      icon: patch.icon !== undefined ? patch.icon : e.icon,
      favorite: patch.favorite !== undefined ? patch.favorite : e.favorite,
      builtin: patch.builtin !== undefined ? patch.builtin : e.builtin,
      hidden: patch.hidden !== undefined ? patch.hidden : e.hidden,
      config: patch.config !== undefined ? patch.config : e.config,
    };
  }

  // 异步提取 exe 图标 → 更新 entry.icon 并持久化 (完成前 UI 显示默认 ▶)
  private fillIcon(path: string): void {
    IconExtractor.getInstance().extract(path).then((icon: string) => {
      if (!icon || !this.has(path)) {
        return;
      }
      this.entries = this.entries.map((e: AppEntry): AppEntry =>
        e.path == path ? this.withField(e, { icon: icon }) : e);
      this.persist();
      this.notify();
    }).catch(() => {});
  }

  remove(path: string): void {
    this.entries = this.entries.filter((entry: AppEntry) => entry.path != path);
    this.persist();
    this.notify();
  }

  toggleFavorite(path: string): void {
    this.entries = this.entries.map((e: AppEntry): AppEntry =>
      e.path == path ? this.withField(e, { favorite: !e.favorite }) : e);
    this.persist();
    this.notify();
  }

  rename(path: string, name: string): void {
    const trimmed = name.trim();
    if (!trimmed) {
      return;
    }
    this.entries = this.entries.map((e: AppEntry): AppEntry =>
      e.path == path ? this.withField(e, { name: trimmed }) : e);
    this.sortEntries();
    this.persist();
    this.notify();
  }

  setHidden(path: string, hidden: boolean): void {
    this.entries = this.entries.map((e: AppEntry): AppEntry =>
      e.path == path ? this.withField(e, { hidden: hidden }) : e);
    this.persist();
    this.notify();
  }

  // prefix 就绪后调用: 内建工具补入库 (幂等; 文件不存在则跳过, 不打扰)。
  // 关键: 无变更时必须静默返回 — 本方法由页面 mirror 在 prefixReady 期间调用,
  // 无条件 notify 会触发 mirror→ensureBuiltinTools→notify 无限递归 (栈溢出
  // 曾被 persist 的 catch 吞成 "persist fail: {}")
  ensureBuiltinTools(): void {
    const driveC = `${WINE_PREFIX}/drive_c`;
    let changed = false;
    // 剔除失效内建条目: 旧版把 regedit 错记进 system32 (存在性检查写错,
    // 见 fileExists), 或 prefix 重建后路径漂移 — 文件已不存在的条目启动
    // 必败、图标永远为空, 清掉后下方按正确路径重新入库
    const kept: AppEntry[] = [];
    for (const e of this.entries) {
      if (e.builtin && !this.fileExists(e.path)) {
        changed = true;
        continue;
      }
      kept.push(e);
    }
    if (changed) {
      this.entries = kept;
    }
    let added = false;
    for (const tool of BUILTIN_TOOLS) {
      const path = `${driveC}/${tool.exe}`;
      const existing = this.entries.find((e: AppEntry) => e.path == path);
      if (existing) {
        // 内建改名迁移: 已入库条目名字持久化在 preferences, 不更新则老设备
        // 永远显示旧名 (如 "Wine 桌面"→"文件管理器")。只动 builtin 条目,
        // 用户自行入库的同路径条目不碰
        if (existing.builtin && existing.name != tool.name) {
          this.entries = this.entries.map((e: AppEntry): AppEntry =>
            e.path == path ? this.withField(e, { name: tool.name }) : e);
          changed = true;
        }
        continue;
      }
      if (!this.fileExists(path)) {
        continue;
      }
      this.entries = this.entries.concat([{ path: path, name: tool.name, builtin: true }]);
      added = true;
      this.fillIcon(path);
    }
    if (!changed && !added) {
      return;
    }
    this.sortEntries();
    this.persist();
    this.notify();
  }

  // accessSync 是布尔语义 (不存在返回 false, 仅权限/参数错误才抛异常),
  // 曾误当 "不存在即抛异常" 写成 try{accessSync}catch{skip}, 检查形同虚设,
  // system32/regedit.exe 因此凭空入库 (该文件从不存在, wine 只装 C:\windows)
  private fileExists(path: string): boolean {
    try {
      return fs.accessSync(path);
    } catch (_) {
      return false;
    }
  }

  has(path: string): boolean {
    for (const entry of this.entries) {
      if (entry.path == path) {
        return true;
      }
    }
    return false;
  }

  // 更新单应用配置 (配置对话框保存, 整体替换; 字段全缺省 = 全跟随全局)
  setConfig(path: string, config: AppConfig): void {
    this.entries = this.entries.map((e: AppEntry): AppEntry =>
      e.path == path ? this.withField(e, { config: config }) : e);
    this.persist();
    this.notify();
  }

  // 启动: 走 runWineProgram (与游戏/smoke 同一条 broker 通道, 逐进程注入 env),
  // 把用户可理解的 AppConfig 翻译成底层 argv/environment/d3dBackend。
  // 无配置时行为与旧 runWineExe 路径等价 (全局 d3dBackend, 无额外 env)。
  launch(path: string): void {
    const env = WineEnvService.getInstance();
    env.setStatusText('启动中...');
    const cfg: AppConfig | undefined = this.entries.find((e: AppEntry) => e.path == path)?.config;
    // 图形渲染: 应用覆盖 → 全局默认。presentBackend 从 d3dBackend 派生
    // (与 WineEnvService.d3dPresentBackend 同规则, 不作为独立用户开关)
    const d3dBackend = cfg?.graphics == 'wined3d' ? 'wined3d'
      : cfg?.graphics == 'dxvk' ? 'dxvk_legacy' : env.getD3dBackend();
    // 兼容模式: box64 经典排障组合 (保守 safeflags + 关闭大块编译, 牺牲速度
    // 换稳定)。经 UpsertEnvLine 覆盖 BuildWineEnv 基线, 只作用于本进程
    const environment: Record<string, string> = {};
    if (cfg?.compat) {
      environment['BOX64_DYNAREC_SAFEFLAGS'] = '2';
      environment['BOX64_DYNAREC_BIGBLOCK'] = '0';
    }
    const winPath = this.toWindowsPath(path, env.zHome);
    testNapi.runWineProgram({
      windowsExePath: winPath,
      argv: this.splitArgs(cfg?.args ?? ''),
      environment: environment,
      workingDirectory: this.windowsParentDir(winPath),
      prefixMode: 'reuse',
      d3dBackend: d3dBackend,
      presentBackend: d3dBackend == 'dxvk_legacy' ? 'venus_broker_present' : 'virgl_compositor',
      automationMode: false
    });
  }

  // native 路径 → Windows 形式: prefix drive_c → C:\, Z: 宿主目录 → Z:\。
  // 都不匹配则原样返回 (native 侧 NativePathToWindows 对 drive_c 再兜底, 幂等)
  private toWindowsPath(path: string, zHome: string): string {
    const driveRoot = `${WINE_PREFIX}/drive_c/`;
    if (path.startsWith(driveRoot)) {
      return `C:\\${path.substring(driveRoot.length).split('/').join('\\')}`;
    }
    // zHome 结尾斜杠归一, 避免 "/Download" 误匹配 "/DownloadApp/..."
    const zRoot = zHome.endsWith('/') ? zHome : `${zHome}/`;
    if (path.startsWith(zRoot)) {
      return `Z:\\${path.substring(zRoot.length).split('/').join('\\')}`;
    }
    return path;
  }

  private windowsParentDir(winPath: string): string {
    const sep = Math.max(winPath.lastIndexOf('\\'), winPath.lastIndexOf('/'));
    return sep > 2 ? winPath.substring(0, sep) : 'C:\\';
  }

  // Windows 形式路径 → native (toWindowsPath 的逆运算): C:\ → prefix drive_c,
  // Z:\ → zHome; 已是 native ("/" 开头) 原样返回; 裸文件名/"desktop" 等无路径
  // 信息的返回 '' (调用方走 basename 匹配兜底)
  nativePathOf(path: string): string {
    if (!path) {
      return '';
    }
    if (path.startsWith('/')) {
      return path;
    }
    const m = /^([A-Za-z]):[\\/](.*)$/.exec(path);
    if (!m) {
      return '';
    }
    const rest = m[2].split('\\').join('/');
    const drive = m[1].toUpperCase();
    if (drive == 'C') {
      return `${WINE_PREFIX}/drive_c/${rest}`;
    }
    if (drive == 'Z') {
      const zHome = WineEnvService.getInstance().zHome;
      const zRoot = zHome.endsWith('/') ? zHome : `${zHome}/`;
      return `${zRoot}${rest}`;
    }
    return '';
  }

  // 运行中进程图标 (任务列表): 应用库条目命中 (规范化路径精确 → basename 忽略
  // 大小写, Windows 语义) 直接借图标; 库外但文件存在则按需提取进会话缓存
  // (不持久化不入库 — explorer 双击启动的 exe 不该污染应用库)。
  // 返回 '' = 暂无; 可提取时已后台触发, 完成后 notify 刷新
  iconForProc(procPath: string): string {
    if (!procPath) {
      return '';
    }
    const nativePath = this.nativePathOf(procPath);
    if (nativePath) {
      const hit = this.entries.find((e: AppEntry) => e.path == nativePath);
      if (hit?.icon) {
        return hit.icon;
      }
    }
    const base = procPath.substring(Math.max(procPath.lastIndexOf('/'), procPath.lastIndexOf('\\')) + 1)
      .toLowerCase();
    if (base) {
      const hit = this.entries.find((e: AppEntry) =>
        e.path.substring(e.path.lastIndexOf('/') + 1).toLowerCase() == base);
      if (hit?.icon) {
        return hit.icon;
      }
    }
    if (nativePath) {
      const cached = this.procIconCache[nativePath];
      if (cached !== undefined) {
        return cached;
      }
      this.extractProcIcon(nativePath);
    }
    return '';
  }

  // 进程图标会话缓存 (nativePath → png, '' = 提取失败不再重试):
  // 应用库外 exe (explorer 内双击 / 游戏子进程), 随进程生命周期, 不持久化
  private procIconCache: Record<string, string> = {};
  private procIconExtracting: string[] = [];

  private extractProcIcon(nativePath: string): void {
    if (this.procIconExtracting.indexOf(nativePath) >= 0 || !this.fileExists(nativePath)) {
      return;
    }
    this.procIconExtracting.push(nativePath);
    IconExtractor.getInstance().extract(nativePath).then((icon: string) => {
      this.procIconExtracting = this.procIconExtracting.filter((p: string) => p != nativePath);
      // 失败也缓存空串, 否则每次 notify 都重试一遍失败提取
      this.procIconCache[nativePath] = icon;
      if (icon) {
        this.notify();
      }
    }).catch(() => {
      this.procIconExtracting = this.procIconExtracting.filter((p: string) => p != nativePath);
      this.procIconCache[nativePath] = '';
    });
  }

  // 启动参数按空白切分 (v1 不支持引号包裹的带空格参数)
  private splitArgs(args: string): string[] {
    return args.trim().split(/\s+/).filter((s: string) => s.length > 0);
  }

  private sortEntries(): void {
    this.entries.sort((a: AppEntry, b: AppEntry) => a.name.localeCompare(b.name));
  }

  private persist(): void {
    if (!this.store) {
      return;
    }
    try {
      this.store.putSync(KEY_ENTRIES, JSON.stringify(this.entries));
      this.store.flush();
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'app library persist fail: %{public}s', JSON.stringify(e));
    }
  }
}
