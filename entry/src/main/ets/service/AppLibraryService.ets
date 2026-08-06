import { hilog } from '@kit.PerformanceAnalysisKit';
import { common } from '@kit.AbilityKit';
import { preferences } from '@kit.ArkData';
import { fileIo as fs } from '@kit.CoreFileKit';
import testNapi from 'libentry.so';
import { WineEnvService, SOCK, WINE_BIN, WINE_LIB } from './WineEnvService';
import { IconExtractor } from './IconExtractor';
import { LnkScanner, LnkTarget } from './LnkScanner';

const DOMAIN = 0x0000;
const STORE_NAME = 'app_library';
const KEY_ENTRIES = 'entries';
const WINE_PREFIX = '/data/storage/el2/base/files/.wine';

export interface AppEntry {
  path: string;
  name: string;
  icon?: string; // 图标 PNG 缓存路径 (IconExtractor 异步提取, 完成前为空, UI 回退 ▶)
  favorite?: boolean;  // 收藏 (筛选 chips)
  lastRun?: number;    // 最近启动时间戳 ms (0/缺省 = 未运行过)
  builtin?: boolean;   // 内建工具 (wine 自带, 角标"内建"; 不可移除/隐藏)
  hidden?: boolean;    // 用户隐藏 (各 chips 均不显示, "已隐藏"chip 可见)
}

// withField 的可选覆盖项 (ArkTS 不用 Partial 以保持显式)
interface AppEntryPatch {
  path?: string;
  name?: string;
  icon?: string;
  favorite?: boolean;
  lastRun?: number;
  builtin?: boolean;
  hidden?: boolean;
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
  { name: 'Wine 桌面', exe: 'windows/system32/explorer.exe' },
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
      lastRun: patch.lastRun !== undefined ? patch.lastRun : e.lastRun,
      builtin: patch.builtin !== undefined ? patch.builtin : e.builtin,
      hidden: patch.hidden !== undefined ? patch.hidden : e.hidden,
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
      if (this.has(path)) {
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

  // 原 Index.doRun: 先置状态提示, 再拉起 exe; 记录 lastRun (最近运行 chip)
  launch(path: string): void {
    const env = WineEnvService.getInstance();
    env.setStatusText('启动中...');
    this.entries = this.entries.map((e: AppEntry): AppEntry =>
      e.path == path ? this.withField(e, { lastRun: Date.now() }) : e);
    this.persist();
    this.notify();
    testNapi.runWineExe(WINE_BIN, SOCK, `${WINE_BIN}:${WINE_LIB}`, path, env.zHome);
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
