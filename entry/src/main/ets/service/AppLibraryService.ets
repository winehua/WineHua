import { hilog } from '@kit.PerformanceAnalysisKit';
import { common } from '@kit.AbilityKit';
import { preferences } from '@kit.ArkData';
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
}

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

  // 异步提取 exe 图标 → 更新 entry.icon 并持久化 (完成前 UI 显示默认 ▶)
  private fillIcon(path: string): void {
    IconExtractor.getInstance().extract(path).then((icon: string) => {
      if (!icon || !this.has(path)) {
        return;
      }
      this.entries = this.entries.map((e: AppEntry): AppEntry =>
        e.path == path ? { path: e.path, name: e.name, icon: icon } : e);
      this.persist();
      this.notify();
    }).catch(() => {});
  }

  remove(path: string): void {
    this.entries = this.entries.filter((entry: AppEntry) => entry.path != path);
    this.persist();
    this.notify();
  }

  has(path: string): boolean {
    for (const entry of this.entries) {
      if (entry.path == path) {
        return true;
      }
    }
    return false;
  }

  // 原 Index.doRun: 先置状态提示, 再拉起 exe
  launch(path: string): void {
    const env = WineEnvService.getInstance();
    env.setStatusText('启动中...');
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
