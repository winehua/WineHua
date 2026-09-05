import { hilog } from '@kit.PerformanceAnalysisKit';
import { common } from '@kit.AbilityKit';
import { preferences } from '@kit.ArkData';
import { fileIo as fs } from '@kit.CoreFileKit';
import { zlib } from '@kit.BasicServicesKit';
import testNapi from 'libentry.so';
import { WineEnvService } from './WineEnvService';
import { BOX64_DYNAREC_PRESETS } from './Box64Dynarec';
import { ProcessInfo } from './ProcessService';
import { IconExtractor } from './IconExtractor';
import { LnkScanner, LnkTarget } from './LnkScanner';

const DOMAIN = 0x0000;
const STORE_NAME = 'app_library';
const KEY_ENTRIES = 'entries';
const WINE_PREFIX = '/data/storage/el2/base/files/.wine';

// Z: 递归扫描上限: 防病态目录树 (海量小文件/死循环软链) 把 UI 线程拖住
const SCAN_MAX_DEPTH = 6;
const SCAN_MAX_EXE = 200;

// 自定义启动临时参数 (弹窗选一次, 不持久化): graphics 渲染策略覆盖,
// compatMode 兼容模式档位 (Box64 dynarec 档位, 对齐 Winlator 4 档, 完整
// 11 参数注入), winver Windows 版本; 字段全可选, 缺省 = 跟随全局 (回落
// 设置页「兼容模式」档位)。ArkTS 禁止内联对象字面量作类型声明, 故抽成显式 interface
export interface LaunchOverrides {
  graphics?: 'dxvk' | 'wined3d';
  compatMode?: 'stability' | 'conservative' | 'intermediate' | 'performance';
  // 启动参数原文 (空白分隔, 如 "-noreactlogin -cef-disable-gpu"): 弹窗当前
  // 输入优先; 缺省回落条目持久化的 launchArgs (上次自定义启动记住的值)
  launchArgs?: string;
  // Windows 版本 (wine 侧 ntdll/version.c version_names 表的合法值, 如
  // 'win95'/'winxp'/'win7'/'win10'/'win11', 全小写精确匹配):
  // winehua 定制版 version_init 优先读环境变量 WINEHUA_WINDOWS_VERSION
  // (移植自 CrossOver CX_WINDOWS_VERSION 补丁), 逐进程生效、不影响已运行
  // 进程, 无需改注册表 (注册表是 prefix 全局的, 临时改会让之后启动的其他
  // 程序串版本)
  winver?: string;
}

export interface AppEntry {
  path: string;
  name: string;
  icon?: string; // 图标 PNG 缓存路径 (IconExtractor 异步提取, 完成前为空, UI 回退 ▶)
  favorite?: boolean;  // 收藏 (正交旗标, 任意来源都可收藏)
  builtin?: boolean;   // 内建工具 (wine 自带, 标签"内建"; 不可移除/隐藏)
  hidden?: boolean;    // 用户隐藏 (各分类均不显示, "已隐藏"分类可见)
  source?: string;     // 入库来源: 'lnk'=开始菜单扫描, 'import'=外部导入 (文件页/Z: 扫描)。
                       // 缺省 (老数据) 按 'import' 归类 — 见 sourceOf, 不做持久化迁移
  lastRun?: number;    // 最近一次受理成功的启动时刻 epoch ms ("最近使用"分类依据;
                       // 只记应用库/文件页经本服务 launch 的启动, explorer 内双击不计)
  launchArgs?: string; // 持久化启动参数 (空白分隔 argv 原文): "自定义启动"弹窗每次
                       // 启动时回写 (清空输入=清除), 普通 launch 未传 overrides 时
                       // 作为缺省参数自动带上 — Steam 等需要固定 flag 的程序靠它免配置
}

// withField 的可选覆盖项 (ArkTS 不用 Partial 以保持显式)
interface AppEntryPatch {
  path?: string;
  name?: string;
  icon?: string;
  favorite?: boolean;
  builtin?: boolean;
  hidden?: boolean;
  source?: string;
  lastRun?: number;
  launchArgs?: string;
}

// 扫描结果: added=新入库条目数, classified=已在库但本次被归到"开始菜单"的数量
// (老数据/手动导入的条目被 lnk 扫描命中时 source 升级为 'lnk' — 该 exe 确实
// 有开始菜单快捷方式, 属事实性归类, 不算新增), removed=本次对账移除的失效
// 条目数 (被扫描来源不再命中: exe 文件 / 快捷方式已删除)。
// 对账只动本次扫描所属来源的条目 (外部导入↔import, 开始菜单↔lnk, 各管各的,
// 不做跨来源迁移 — exe 仍在 Z: 的失效 lnk 条目由下次 scanZExes 以 import 重新入库)
export interface ScanMarks {
  added: number;
  classified: number;
  removed: number;
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
  { name: '控制面板', exe: 'windows/system32/control.exe' },
  // wine 自带 IE 浏览器 (mshtml/jscript/vbscript 内建可用); 文件名是
  // iexplore.exe (常见误写 iexplorer), 经 FakeDlls 装进 system32
  { name: '浏览器', exe: 'windows/system32/iexplore.exe' },
  // wine 自带小工具 (system32 的 exe stub, 设备实测存在)
  { name: '扫雷', exe: 'windows/system32/winemine.exe' },
  { name: 'winefile', exe: 'windows/system32/winefile.exe' },
  // 烟测程序 (C:\smoke 在持久 prefix 内, 每次启动由 syncManagedSmoke 刷新)
  { name: 'Graphics Smoke', exe: 'smoke/x64/winehua_graphics_smoke.exe' },
  { name: 'D3d Smoke', exe: 'smoke/x64/winehua_d3d_switch_cube.exe' },
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

  // 扫描进行中状态 (服务持有, 与 WineEnvService.initOverlayText 同款先例):
  // Index 根 Stack 据此渲染全窗遮罩 — 遮罩必须盖住侧栏, 不能由各页面自管
  // (曾用页面内 bindContentCover, 实测不弹出的同时触摸拦截范围也只覆盖内容区)
  scanning: boolean = false;
  scanLabel: string = '';

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
    this.ctx = ctx;
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

  // ctx 保存 (ensureSteamLegacy 的 resourceManager 依赖)
  private ctx: common.Context | null = null;
  // Steam Legacy 解压进行中标记 (防重入; ensureBuiltinTools 每次 prefixReady 都调)
  private steamLegacyBusy = false;

  // Steam Legacy 预置客户端: rawfile/steam-legacy.zip (6243 文件, 716MB,
  // 内部路径根为 "Program Files (x86)/Steam/") → 解压到 prefix drive_c。
  // 幂等: marker = Steam/steam.exe 已存在即跳过。fire-and-forget 后台执行,
  // 解压完成前点 Steam 条目会启动失败 (文件还没落地)。
  // 与 WineEnvService.extractWineData 同款流式拷贝 (getRawFdSync 分块读,
  // 避免 rawfile 一次性加载进内存)。
  private ensureSteamLegacy(): void {
    if (this.steamLegacyBusy || !this.ctx) {
      return;
    }
    const marker = `${WINE_PREFIX}/drive_c/Program Files (x86)/Steam/steam.exe`;
    if (this.fileExists(marker)) {
      return;
    }
    this.steamLegacyBusy = true;
    // OHOS zlib.decompressFile 对 zip 条目路径中的空格会报 900001, 故 zip 内
    // 路径根为无空格的 "Steam/", 解压到 files/ 后整体 rename 进 prefix
    const filesBase = WINE_PREFIX.substring(0, WINE_PREFIX.lastIndexOf('/'));
    const srcDir = `${filesBase}/Steam`;
    const destDir = `${WINE_PREFIX}/drive_c/Program Files (x86)/Steam`;
    const tmpZip = `${filesBase}/steam-legacy-tmp.zip`;
    (async (): Promise<void> => {
      try {
        const rm = this.ctx!.resourceManager;
        const rawFd = rm.getRawFdSync('steam-legacy.zip');
        hilog.warn(DOMAIN, 'wine', 'steam-legacy: extracting (%{public}d bytes)...', rawFd.length);
        // 流式拷贝 rawfile → 临时 zip (4MB 分块)
        const dst = fs.openSync(tmpZip, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
        const CHUNK = 4 * 1024 * 1024;
        const buf = new ArrayBuffer(CHUNK);
        let pos = rawFd.offset;
        const end = rawFd.offset + rawFd.length;
        while (pos < end) {
          const toRead = Math.min(CHUNK, end - pos);
          const n = fs.readSync(rawFd.fd, buf, { offset: pos, length: toRead });
          if (n <= 0) {
            break;
          }
          fs.writeSync(dst.fd, buf, { length: n });
          pos += n;
        }
        fs.closeSync(dst);
        rm.closeRawFdSync('steam-legacy.zip');
        // 解压到 files/ (无空格路径), 再整体改名挪进 prefix
        await zlib.decompressFile(tmpZip, filesBase, {});
        try {
          fs.rmdirSync(destDir);
        } catch (_) {}
        fs.renameSync(srcDir, destDir);
        hilog.warn(DOMAIN, 'wine', 'steam-legacy: extracted, marker=%{public}d', this.fileExists(marker) ? 1 : 0);
      } catch (e) {
        hilog.error(DOMAIN, 'wine', 'steam-legacy extract fail: %{public}s', JSON.stringify(e));
      } finally {
        try {
          fs.unlinkSync(tmpZip);
        } catch (_) {}
        this.steamLegacyBusy = false;
      }
    })();
  }

  // dnsapi unixlib 补丁进行中标记
  private dnsapiPatchBusy = false;

  // dnsapi unixlib 补丁: OHOS musl 无 libresolv (res_query 等全是桩符号),
  // 上游 HAVE_RESOLV 检测不过, 产出的 x86_64-unix/dnsapi.so 是空壳, 运行时
  // DnsQuery_A 直接 0xC0000005。rawfile/dnsapi-fix.zip (条目根 wine/bin/
  // x86_64-unix/dnsapi.so) 是 musl 适配版 (resolv.conf + UDP 查询 + getaddrinfo
  // 兜底)。幂等 marker = 目标文件大小为补丁版本 (空壳 9752 / 补丁 18472,
  // 大小不同即重打)。解压根 files/ 与 zip 条目根正好对齐, 无需 rename。
  private ensureDnsapiPatch(): void {
    if (this.dnsapiPatchBusy || !this.ctx) {
      return;
    }
    const filesBase = WINE_PREFIX.substring(0, WINE_PREFIX.lastIndexOf('/'));
    const target = `${filesBase}/wine/bin/x86_64-unix/dnsapi.so`;
    const marker = `${filesBase}/dnsapi-fix-applied`;
    // 已应用 (marker 文件) 且目标仍在 → 跳过; 目标丢失 (wine 数据被清) 时
    // 连 marker 一起重来
    if (this.fileExists(marker)) {
      try {
        fs.statSync(target);
        return;
      } catch (_) {
        try {
          fs.unlinkSync(marker);
        } catch (_) {}
      }
    }
    this.dnsapiPatchBusy = true;
    const tmpZip = `${filesBase}/dnsapi-fix-tmp.zip`;
    (async (): Promise<void> => {
      try {
        const rm = this.ctx!.resourceManager;
        const rawFd = rm.getRawFdSync('dnsapi-fix.zip');
        const dst = fs.openSync(tmpZip, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
        const CHUNK = 1024 * 1024;
        const buf = new ArrayBuffer(CHUNK);
        let pos = rawFd.offset;
        const end = rawFd.offset + rawFd.length;
        while (pos < end) {
          const toRead = Math.min(CHUNK, end - pos);
          const n = fs.readSync(rawFd.fd, buf, { offset: pos, length: toRead });
          if (n <= 0) {
            break;
          }
          fs.writeSync(dst.fd, buf, { length: n });
          pos += n;
        }
        fs.closeSync(dst);
        rm.closeRawFdSync('dnsapi-fix.zip');
        await zlib.decompressFile(tmpZip, filesBase, {});
        const st = fs.statSync(target);
        const mk = fs.openSync(marker, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY);
        fs.closeSync(mk);
        hilog.warn(DOMAIN, 'wine', 'dnsapi-patch: applied, size=%{public}d', st.size);
      } catch (e) {
        hilog.error(DOMAIN, 'wine', 'dnsapi-patch fail: %{public}s', JSON.stringify(e));
      } finally {
        try {
          fs.unlinkSync(tmpZip);
        } catch (_) {}
        this.dnsapiPatchBusy = false;
      }
    })();
  }

  // 诊断工具投放: rawfile/diagtools.zip (条目根 .wine/drive_c/windows/temp/
  // chunkfetch2.exe) — P1 下载解包失败的独立抓块对比实验用 (game 通道以
  // C:\windows\temp\chunkfetch2.exe + URL 参数启动, 不经 steam 下载器)。
  // 与 dnsapi 补丁同模式: marker 幂等, 目标丢失自动重解压。
  private ensureDiagTools(): void {
    if (!this.ctx) {
      return;
    }
    const filesBase = WINE_PREFIX.substring(0, WINE_PREFIX.lastIndexOf('/'));
    const tempDir = `${filesBase}/.wine/drive_c/windows/temp`;
    const target = `${tempDir}/chunkfetch2.exe`;
    const marker = `${filesBase}/diag-tools-applied-v6`;
    if (this.fileExists(marker)) {
      try {
        fs.statSync(target);
        fs.statSync(`${tempDir}/steamlaunch.exe`);
        fs.statSync(`${tempDir}/steamlaunch2.exe`);
        fs.statSync(`${tempDir}/sapmover.exe`);
        fs.statSync(`${tempDir}/aesni_test.exe`);
        fs.statSync(`${tempDir}/winexec.exe`);
        fs.statSync(`${tempDir}/rjlaunch.exe`);
        fs.statSync(`${tempDir}/spawntest.exe`);
        fs.statSync(`${tempDir}/rjfix.exe`);
        fs.statSync(`${tempDir}/appmanifest_1714040.acf`);
        return;
      } catch (_) {
        try {
          fs.unlinkSync(marker);
        } catch (_) {}
      }
    }
    const tmpZip = `${filesBase}/diagtools-tmp.zip`;
    (async (): Promise<void> => {
      try {
        const rm = this.ctx!.resourceManager;
        const rawFd = rm.getRawFdSync('diagtools.zip');
        const dst = fs.openSync(tmpZip, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
        const CHUNK = 1024 * 1024;
        const buf = new ArrayBuffer(CHUNK);
        let pos = rawFd.offset;
        const end = rawFd.offset + rawFd.length;
        while (pos < end) {
          const toRead = Math.min(CHUNK, end - pos);
          const n = fs.readSync(rawFd.fd, buf, { offset: pos, length: toRead });
          if (n <= 0) {
            break;
          }
          fs.writeSync(dst.fd, buf, { length: n });
          pos += n;
        }
        fs.closeSync(dst);
        rm.closeRawFdSync('diagtools.zip');
        await zlib.decompressFile(tmpZip, filesBase, {});
        fs.statSync(target);
        fs.statSync(`${tempDir}/steamlaunch.exe`);
        fs.statSync(`${tempDir}/steamlaunch2.exe`);
        fs.statSync(`${tempDir}/sapmover.exe`);
        fs.statSync(`${tempDir}/aesni_test.exe`);
        fs.statSync(`${tempDir}/winexec.exe`);
        fs.statSync(`${tempDir}/rjlaunch.exe`);
        fs.statSync(`${tempDir}/spawntest.exe`);
        fs.statSync(`${tempDir}/rjfix.exe`);
        fs.statSync(`${tempDir}/appmanifest_1714040.acf`);
        const mk = fs.openSync(marker, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY);
        fs.closeSync(mk);
        hilog.warn(DOMAIN, 'wine', 'diag-tools: chunkfetch2 staged');
      } catch (e) {
        hilog.error(DOMAIN, 'wine', 'diag-tools fail: %{public}s', JSON.stringify(e));
      } finally {
        try {
          fs.unlinkSync(tmpZip);
        } catch (_) {}
      }
    })();
  }

  // 一次性游戏搬运 (rawfile/game-transfer.zip): Super Auto Pets 离线安装包
  // (条目根 = .wine/drive_c/Program Files (x86)/Steam/steamapps/...)。
  // P1 绕过方案: 设备下载管线对特定 chunk 内容必败, 验证完整性只读本地
  // 文件, 搬运后 Steam 即认已安装。幂等 marker=game-transfer-applied,
  // 清单丢失 (prefix 重建) 自动重解压。
  private ensureGameTransfer(): void {
    if (!this.ctx) {
      return;
    }
    const filesBase = WINE_PREFIX.substring(0, WINE_PREFIX.lastIndexOf('/'));
    const target = `${filesBase}/.wine/drive_c/Program Files (x86)/Steam/steamapps/appmanifest_1714040.acf`;
    const marker = `${filesBase}/game-transfer-applied`;
    if (this.fileExists(marker)) {
      try {
        fs.statSync(target);
        return;
      } catch (_) {
        try {
          fs.unlinkSync(marker);
        } catch (_) {}
      }
    }
    const tmpZip = `${filesBase}/game-transfer-tmp.zip`;
    (async (): Promise<void> => {
      try {
        const rm = this.ctx!.resourceManager;
        const rawFd = rm.getRawFdSync('game-transfer.zip');
        const dst = fs.openSync(tmpZip, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
        const CHUNK = 1024 * 1024;
        const buf = new ArrayBuffer(CHUNK);
        let pos = rawFd.offset;
        const end = rawFd.offset + rawFd.length;
        while (pos < end) {
          const toRead = Math.min(CHUNK, end - pos);
          const n = fs.readSync(rawFd.fd, buf, { offset: pos, length: toRead });
          if (n <= 0) {
            break;
          }
          fs.writeSync(dst.fd, buf, { length: n });
          pos += n;
        }
        fs.closeSync(dst);
        rm.closeRawFdSync('game-transfer.zip');
        await zlib.decompressFile(tmpZip, filesBase, {});
        fs.statSync(target);
        const mk = fs.openSync(marker, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY);
        fs.closeSync(mk);
        hilog.warn(DOMAIN, 'wine', 'game-transfer: Super Auto Pets installed');
      } catch (e) {
        hilog.error(DOMAIN, 'wine', 'game-transfer fail: %{public}s', JSON.stringify(e));
      } finally {
        try {
          fs.unlinkSync(tmpZip);
        } catch (_) {}
      }
    })();
  }

  // name 取文件名去 .exe 后缀
  private exeNameOf(path: string): string {
    const fileName = path.substring(path.lastIndexOf('/') + 1);
    return fileName.toLowerCase().endsWith('.exe') ? fileName.substring(0, fileName.length - 4) : fileName;
  }

  // 按 path 去重, 已存在则忽略
  add(path: string): void {
    this.addNamed(path, this.exeNameOf(path));
  }

  addNamed(path: string, name: string, source: string = 'import'): void {
    if (this.has(path)) {
      return;
    }
    this.entries = this.entries.concat([{ path: path, name: name, source: source }]);
    this.sortEntries();
    this.persist();
    this.notify();
    this.fillIcon(path);
  }

  // 扫描 Start Menu / Desktop 的 .lnk 入库 (M3, 手动触发; 异步 IO 不卡 UI)。
  // 已在库的非内建条目被命中时 source 升级为 'lnk' (该 exe 确有开始菜单
  // 快捷方式, 属事实性归类; 用户手动 import 的同路径条目也照此处理)。
  // 对账: source 为 'lnk' 但本次不再被任何快捷方式命中的条目直接移除 —
  // .lnk 被删 (程序卸载/快捷方式删除), 或指向的 exe 已删 (LnkScanner 解析
  // 不出目标, 不进 found) 都会落到这里。与外部导入各管各的: 若 exe 文件
  // 仍在 Z:, 下次 scanZExes 会以 import 重新入库, 这里不做跨来源迁移。
  async scanShortcuts(): Promise<ScanMarks> {
    this.beginScan('正在扫描开始菜单…');
    try {
      const found: LnkTarget[] = await LnkScanner.getInstance().scan(WINE_PREFIX, WineEnvService.getInstance().zHome);
      const marks: ScanMarks = { added: 0, classified: 0, removed: 0 };
      const fresh: AppEntry[] = [];
      const upgraded: string[] = [];
      for (const t of found) {
        const existing = this.entries.find((e: AppEntry) => e.path == t.target);
        if (!existing) {
          // 同次扫描 Desktop 与 Start Menu 可能命中同目标, fresh 内也要去重
          if (!fresh.some((f: AppEntry) => f.path == t.target)) {
            fresh.push({ path: t.target, name: t.name, source: 'lnk' });
            marks.added++;
          }
          continue;
        }
        if (!existing.builtin && existing.source != 'lnk' && upgraded.indexOf(t.target) < 0) {
          upgraded.push(t.target);
          marks.classified++;
        }
      }
      // 对账移除: 只在本次命中集合之外且来源为 lnk 的条目上操作。
      // upgraded 条目的 source 此刻还是原值 (import), 不会误入移除
      const foundTargets = new Set(found.map((t: LnkTarget) => t.target));
      this.entries = this.entries.filter((e: AppEntry) => {
        if (this.sourceOf(e) == 'lnk' && !foundTargets.has(e.path)) {
          marks.removed++;
          return false;
        }
        return true;
      });
      // 新增 + 归类升级 + 对账移除 → 统一一次 sort/persist/notify
      // (逐条操作会让整组 JSON flush 与整页重渲染各跑 N 遍, 200 条目
      // 是 4 万次级写放大, UI 明显抖动)
      if (fresh.length > 0 || upgraded.length > 0 || marks.removed > 0) {
        if (upgraded.length > 0) {
          this.entries = this.entries.map((e: AppEntry): AppEntry =>
            upgraded.indexOf(e.path) >= 0 ? this.withField(e, { source: 'lnk' }) : e);
        }
        if (fresh.length > 0) {
          this.entries = this.entries.concat(fresh);
        }
        this.sortEntries();
        this.persist();
        this.notify();
      }
      for (const e of fresh) {
        this.fillIcon(e.path);
      }
      return marks;
    } finally {
      this.endScan();
    }
  }

  // 扫描 Z: (zHome 下载目录) 下的 .exe 入库为"外部导入" (手动触发; 异步 IO)。
  // 递归遍历, 跳过 . 开头目录; 深度/总量设上限 (SCAN_MAX_DEPTH/SCAN_MAX_EXE)
  // 防病态目录树拖住扫描。已在库 (含隐藏) 的跳过。
  // 对账: source 为 'import' 且文件已不存在的条目移除。found 集合受扫描上限
  // 截断影响 (超深/超量漏扫), 未命中但文件仍在的不删 — 叠加 fileExists 双保险;
  // 收藏/隐藏的失效条目也一律移除 (文件没了启动必败)。只动 import 来源,
  // lnk 条目由 scanShortcuts 对账, 各管各的 (exe 仍在 Z: 的失效 lnk 条目
  // 会在下次 scanShortcuts 移除后由本扫描以 import 重新入库)
  async scanZExes(): Promise<ScanMarks> {
    const zHome = WineEnvService.getInstance().zHome;
    if (!zHome) {
      return { added: 0, classified: 0, removed: 0 };
    }
    this.beginScan('正在扫描 Z: 目录…');
    try {
      const root = zHome.endsWith('/') ? zHome.substring(0, zHome.length - 1) : zHome;
      const found: string[] = [];
      await this.collectExe(root, 0, found);
      // 对账移除 (先于新增判断, 保证下方 has() 反映移除后的库)
      const foundSet = new Set(found);
      let removed = 0;
      this.entries = this.entries.filter((e: AppEntry) => {
        if (this.sourceOf(e) == 'import' && !foundSet.has(e.path) && !this.fileExists(e.path)) {
          removed++;
          return false;
        }
        return true;
      });
      const fresh: AppEntry[] = [];
      for (const p of found) {
        if (!this.has(p)) {
          fresh.push({ path: p, name: this.exeNameOf(p), source: 'import' });
        }
      }
      // 新增 + 对账移除 → 统一一次 sort/persist/notify
      if (fresh.length > 0 || removed > 0) {
        if (fresh.length > 0) {
          this.entries = this.entries.concat(fresh);
        }
        this.sortEntries();
        this.persist();
        this.notify();
      }
      for (const e of fresh) {
        this.fillIcon(e.path);
      }
      return { added: fresh.length, classified: 0, removed: removed };
    } finally {
      this.endScan();
    }
  }

  // 扫描状态开合 (try/finally 保证异常路径也复位): 置位即 notify,
  // Index 根 Stack 的全窗遮罩跟随显隐
  private beginScan(label: string): void {
    this.scanning = true;
    this.scanLabel = label;
    this.notify();
  }

  private endScan(): void {
    this.scanning = false;
    this.notify();
  }

  private async collectExe(dir: string, depth: number, out: string[]): Promise<void> {
    if (depth > SCAN_MAX_DEPTH || out.length >= SCAN_MAX_EXE) {
      return;
    }
    let names: string[];
    try {
      names = await fs.listFile(dir);
    } catch (e) {
      return; // 目录不可读属正常 (权限/已删除), 跳过
    }
    for (const name of names) {
      if (out.length >= SCAN_MAX_EXE) {
        return;
      }
      if (name.startsWith('.')) {
        continue;
      }
      const full = `${dir}/${name}`;
      let isDir = false;
      try {
        isDir = (await fs.stat(full)).isDirectory();
      } catch (e) {
        continue;
      }
      if (isDir) {
        await this.collectExe(full, depth + 1, out);
      } else if (name.toLowerCase().endsWith('.exe')) {
        out.push(full);
      }
    }
  }

  // 条目来源归类: 内建看 builtin 旗标, 其余看 source 字段;
  // 老数据无 source 一律按 'import' (免持久化迁移, lnk 条目重扫后自然归位)
  sourceOf(e: AppEntry): string {
    if (e.builtin) {
      return 'builtin';
    }
    return e.source ?? 'import';
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
      source: patch.source !== undefined ? patch.source : e.source,
      lastRun: patch.lastRun !== undefined ? patch.lastRun : e.lastRun,
      launchArgs: patch.launchArgs !== undefined ? patch.launchArgs : e.launchArgs,
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
    // Steam Legacy 预置解压 (幂等自带 marker 检查, 置顶触发 — 下方无变更
    // 早退分支不能挡它, 否则第二次启动永远不解压)
    this.ensureSteamLegacy();
    this.ensureDnsapiPatch();
    this.ensureDiagTools();
    this.ensureGameTransfer();
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

  // 兼容模式档位 + WEAKBARRIER clamp 统一应用 (launch / openInWine 共用):
  // compatPreset 缺省 = 不注入 (走基线); clamp — DXVK/vkd3d 后端强制
  // WEAKBARRIER=0: 档位注入的弱屏障经 UpsertEnvLine 最后应用优先级最高,
  // 会覆盖掉 DXVK 应有的 0 — Venus ring 期望 x86 定序, 激进的弱屏障会损坏
  // 共享内存命令环 (随机 Vulkan 损坏); 必须作为最后写入 (档位注入之后)。
  // wined3d 无 ring, 不 clamp。注意: 本 clamp 判据覆盖 vkd3d_limited_500k,
  // 与 d3dLaunchEnvironment 不同 (该函数仅在 dxvk_* 分支 clamp) — 这与 native
  // 会话链 (wine_env.cpp vkd3d 分支 / AppendStableDesktopDxvkEnv) 对 vkd3d
  // 同样 clamp 的口径一致, 直启路径此前漏了 vkd3d 才是缺口。
  private applyCompatAndClamp(environment: Record<string, string>, d3dBackend: string,
    compatPreset: Record<string, string> | undefined): void {
    if (compatPreset) {
      for (const k of Object.keys(compatPreset)) {
        environment[k] = compatPreset[k];
      }
    }
    if (d3dBackend.startsWith('dxvk') || d3dBackend == 'vkd3d_limited_500k') {
      environment['BOX64_DYNAREC_WEAKBARRIER'] = '0';
    }
  }

  // 持久化条目启动参数 ("自定义启动"弹窗每次启动时回写; 清空输入 = 清除)。
  // 条目不在库 (文件页库外直启) 时静默跳过 — map 自然空转
  setLaunchArgs(path: string, args: string): void {
    const normalized = args.trim();
    this.entries = this.entries.map((e: AppEntry): AppEntry =>
      e.path == path ? this.withField(e, { launchArgs: normalized }) : e);
    this.persist();
  }

  // "启动参数"字符串 → argv 数组: 按空白切分, 多个空格/制表符都算分隔
  // (不支持带引号的含空格参数 — 启动 flag 场景用不到); 空串/纯空白 → []
  private parseLaunchArgs(s: string): string[] {
    const trimmed = s.trim();
    if (!trimmed) {
      return [];
    }
    const argv: string[] = [];
    const parts = trimmed.split(/\s+/);
    for (const p of parts) {
      if (p) {
        argv.push(p);
      }
    }
    return argv;
  }

  // 启动: 走 runWineProgram (与游戏/smoke 同一条 broker 通道, 逐进程注入 env)。
  // overrides 为"自定义启动"临时参数 (graphics/compatMode/winver 弹窗选一次
  // 不持久化; launchArgs 例外 — 视图层在 launch 前经 setLaunchArgs 回写条目,
  // 之后普通启动也自动带上):
  //   graphics    'wined3d'→WineD3D, 'dxvk'→DXVK; undefined = 跟随全局 d3dBackend
  //   compatMode  'stability'/'conservative'/'intermediate'/'performance'
  //               (对齐 Winlator 4 档位, 完整 11 参数注入);
  //               undefined = 跟随全局 (回落设置页「兼容模式」档位注入)
  // 覆盖经 UpsertEnvLine 压过 BuildWineEnv 基线, 只作用于本进程。
  // 返回 broker 受理的 pid (<=0 = 受理失败, 调用方立即报启动失败);
  // 受理只表示进程拉起, 闪退检测由调用方订阅 evt:proc-exited 完成
  launch(path: string, overrides?: LaunchOverrides): number {
    const env = WineEnvService.getInstance();
    // 图形渲染: 临时覆盖 → 全局默认。presentBackend 从 d3dBackend 派生
    // (与 WineEnvService.d3dPresentBackend 同规则, 不作为独立用户开关)
    const d3dBackend = overrides?.graphics == 'wined3d' ? 'wined3d'
      : overrides?.graphics == 'dxvk' ? 'dxvk_legacy' : env.getD3dBackend();
    const environment: Record<string, string> = {};
    // 语言设置对程序直启逐进程注入 (桌面会话内由 explorer 继承, 此处兜底
    // 覆盖 BuildWineEnv 基线); UpsertEnvLine 保证它压过 native 默认值。
    // 必须 LANG 与 LC_ALL 同设: musl 无 locale 数据, setlocale(LANG) 失败返回
    // "C", Wine 只读 LC_ALL 兜底解析 LCID — 只改 LANG 则 LC_ALL 停留 BuildWineEnv
    // 默认 zh_CN, 程序永远是中文
    environment['LANG'] = `${env.getWineLang()}.UTF-8`;
    environment['LC_ALL'] = `${env.getWineLang()}.UTF-8`;
    // 兼容模式档位 → BOX64 dynarec 覆盖 (仅 arm64 生效): 弹窗选中档位优先
    // (临时覆盖), 否则跟随设置页全局档位 ("跟随全局"语义);
    // 都未配置 = 不注入, 走出厂基线 SAFEFLAGS=1+BIGBLOCK=3+CALLRET=2+
    // FORWARD=1024+WEAKBARRIER=2
    const overridePreset = overrides?.compatMode ? BOX64_DYNAREC_PRESETS[overrides.compatMode] : undefined;
    const globalMode = env.getCompatMode();
    const compatPreset = overridePreset ?? (globalMode ? BOX64_DYNAREC_PRESETS[globalMode] : undefined);
    // 档位注入 + DXVK/vkd3d WEAKBARRIER=0 clamp 统一收口 (见 applyCompatAndClamp)
    this.applyCompatAndClamp(environment, d3dBackend, compatPreset);
    // Windows 版本覆盖: 注入 WINEHUA_WINDOWS_VERSION (ntdll/version_init 优先读
    // 它, 回落注册表), 只作用于本进程; 两架构都生效 (版本逻辑在 ntdll 层)
    const winver = overrides?.winver;
    if (winver) {
      environment['WINEHUA_WINDOWS_VERSION'] = winver;
    }
    // 启动参数 → argv: 弹窗当次输入优先, 缺省回落条目持久化的 launchArgs
    // (上次自定义启动记住的值); 都没有 = 空数组 (runWineProgram 原样透传)
    const entry = this.entries.find((e: AppEntry) => e.path == path);
    const argsSrc = overrides?.launchArgs ?? entry?.launchArgs ?? '';
    const argv: string[] = this.parseLaunchArgs(argsSrc);
    // 注意: 不可为 steam 注入任何 WINEDEBUG 调试通道 (+server/+font 等 stderr
    // 洪水会塞满管道把整个会话拖死, 2026-09-04 三次冻结的根因)。
    const winPath = this.toWindowsPath(path, env.zHome);
    const result = testNapi.runWineProgram({
      windowsExePath: winPath,
      argv: argv,
      environment: environment,
      workingDirectory: this.windowsParentDir(winPath),
      prefixMode: 'reuse',
      d3dBackend: d3dBackend,
      dxvkBackend: env.getDxvkBackend(),
      // 与 WineEnvService.d3dPresentBackend 同规则: dxvk_* / vkd3d 走 Venus
      // broker 呈现, 仅 wined3d 回落 virgl 离屏合成
      presentBackend: d3dBackend.startsWith('dxvk_') || d3dBackend == 'vkd3d_limited_500k' ?
        'venus_broker_present' : 'virgl_compositor',
      automationMode: false
    }) as object as ProcessInfo;
    if (result && result.pid > 0) {
      this.markLaunched(path);
      return result.pid;
    }
    return -1;
  }

  // 在 Wine 内打开目录: 以 explorer.exe 启动指定目录 (Windows 路径),
  // 弹出 Wine 侧的文件管理器窗口。复用 launch 的环境/后端编排,
  // 仅 exe 与 argv 不同。目录可来自 C: 或 Z:
  openInWine(dirPath: string): number {
    const env = WineEnvService.getInstance();
    const d3dBackend = env.getD3dBackend();
    const environment: Record<string, string> = {};
    // 与 launch 同款语言兜底注入 (explorer 新实例独立进程, 不走桌面继承)
    environment['LANG'] = `${env.getWineLang()}.UTF-8`;
    environment['LC_ALL'] = `${env.getWineLang()}.UTF-8`;
    // 兼容模式全局档位 (无 overrides, 始终跟随设置页档位) + 图形 clamp:
    // 经文件页打开目录拉起的 explorer 也应拿到档位, 与 launch 同款收口
    const compatMode = env.getCompatMode();
    this.applyCompatAndClamp(environment, d3dBackend,
      compatMode ? BOX64_DYNAREC_PRESETS[compatMode] : undefined);
    const winDir = this.toWindowsPath(dirPath, env.zHome);
    const result = testNapi.runWineProgram({
      windowsExePath: 'C:\\windows\\system32\\explorer.exe',
      argv: [winDir],
      environment: environment,
      workingDirectory: winDir,
      prefixMode: 'reuse',
      d3dBackend: d3dBackend,
      dxvkBackend: env.getDxvkBackend(),
      // 与 WineEnvService.d3dPresentBackend 同规则: dxvk_* / vkd3d 走 Venus
      // broker 呈现, 仅 wined3d 回落 virgl 离屏合成
      presentBackend: d3dBackend.startsWith('dxvk_') || d3dBackend == 'vkd3d_limited_500k' ?
        'venus_broker_present' : 'virgl_compositor',
      automationMode: false
    }) as object as ProcessInfo;
    return (result && result.pid > 0) ? result.pid : -1;
  }

  // 从"最近使用"移除: 清掉 lastRun (不带该字段 = recent 分类消失)。条目本身
  // 保留在库 — 收藏/开始菜单/来源分类不受影响, 下次启动重新入列。
  // 不能用 withField({lastRun: undefined}): 它的 undefined 语义是保留原值,
  // 故内联构造 (对象字面量显式列全字段, 保持 ArkTS 风格)
  clearRecent(path: string): void {
    this.entries = this.entries.map((e: AppEntry): AppEntry => {
      if (e.path != path || e.lastRun === undefined) {
        return e;
      }
      return {
        path: e.path,
        name: e.name,
        icon: e.icon,
        favorite: e.favorite,
        builtin: e.builtin,
        hidden: e.hidden,
        source: e.source,
        launchArgs: e.launchArgs
      };
    });
    this.persist();
    this.notify();
  }

  // 记录启动时刻 ("最近使用"分类的排序依据)。仅受理成功才记 — 受理失败
  // /闪退不代表用户真的"用"了它; 库外路径 (文件页直接启动未入库 exe) 跳过
  private markLaunched(path: string): void {
    if (!this.has(path)) {
      return;
    }
    const now = Date.now();
    this.entries = this.entries.map((e: AppEntry): AppEntry =>
      e.path == path ? this.withField(e, { lastRun: now }) : e);
    this.persist();
    this.notify();
  }

  // native 路径 → Windows 形式: prefix drive_c → C:\, Z: 宿主目录 → Z:\。
  // 都不匹配则原样返回 (native 侧 NativePathToWindows 对 drive_c 再兜底, 幂等)
  private toWindowsPath(path: string, zHome: string): string {
    const driveRoot = `${WINE_PREFIX}/drive_c/`;
    if (path.startsWith(driveRoot)) {
      return `C:\\${path.substring(driveRoot.length).split('/').join('\\')}`;
    }
    // zHome 结尾斜杠归一, 避免 "/Download" 误匹配 "/DownloadApp/..."
    // zHome 为空时 zRoot 置 '' — 空串是任意路径前缀, 会把所有 native 路径
    // ("/" 开头) 误判成 Z: 盘, 此时跳过 Z: 分支保持原生
    const zRoot = zHome ? (zHome.endsWith('/') ? zHome : `${zHome}/`) : '';
    if (zRoot && path.startsWith(zRoot)) {
      return `Z:\\${path.substring(zRoot.length).split('/').join('\\')}`;
    }
    return path;
  }

  // UI 展示用: native 绝对路径 → Windows 形式 (C:\ / Z:\)。
  // zHome 未就绪时 C: 盘仍正确转换 (不依赖 zHome); Z: 盘暂保持原生, zHome
  // 就绪后页面订阅 env notify 会自动刷新为 Z:\ 形式
  windowsPathOf(path: string): string {
    return this.toWindowsPath(path, WineEnvService.getInstance().zHome);
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
