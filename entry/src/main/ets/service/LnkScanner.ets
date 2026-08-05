import { fileIo as fs } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0x0000;
const TAG = 'LnkScanner';

export interface LnkTarget {
  name: string;   // 显示名 (lnk 内 NAME_STRING, 缺省用文件名去后缀)
  target: string; // 解析后的 native exe 路径
}

// .lnk (Shell Link Binary, [MS-SHLLINK]) 扫描与解析。
// 扫描 Start Menu (ProgramData + 各用户) 与 Desktop (Public + 各用户) 四个根,
// 解析 LinkInfo 的 LocalBasePath(Unicode) 拿目标路径, C:\ → drive_c、Z:\ → zHome
// 逐段大小写不敏感解析 (lnk 里的路径大小写与磁盘目录可能不一致)。
export class LnkScanner {
  private static instance: LnkScanner | null = null;

  static getInstance(): LnkScanner {
    if (!LnkScanner.instance) {
      LnkScanner.instance = new LnkScanner();
    }
    return LnkScanner.instance;
  }

  // prefixDir: wine prefix 根 (…/files/.wine); zHome: Z: 映射的宿主目录
  scan(prefixDir: string, zHome: string): LnkTarget[] {
    const driveC = `${prefixDir}/drive_c`;
    const roots: string[] = [
      `${driveC}/ProgramData/Microsoft/Windows/Start Menu/Programs`,
      `${driveC}/users/Public/Desktop`
    ];
    try {
      const users = fs.listFileSync(`${driveC}/users`);
      for (const u of users) {
        if (u == 'Public') {
          continue;
        }
        roots.push(`${driveC}/users/${u}/AppData/Roaming/Microsoft/Windows/Start Menu/Programs`);
        roots.push(`${driveC}/users/${u}/Desktop`);
      }
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'list users fail: %{public}s', JSON.stringify(e));
    }
    const out: LnkTarget[] = [];
    for (const root of roots) {
      this.scanDir(root, driveC, zHome, out);
    }
    hilog.info(DOMAIN, TAG, 'scan done: %{public}d usable lnk(s)', out.length);
    return out;
  }

  private scanDir(dir: string, driveC: string, zHome: string, out: LnkTarget[]): void {
    let names: string[];
    try {
      names = fs.listFileSync(dir);
    } catch (e) {
      return; // 目录不存在属正常 (未安装任何程序时)
    }
    for (const name of names) {
      const full = `${dir}/${name}`;
      let isDir = false;
      try {
        isDir = fs.statSync(full).isDirectory();
      } catch (e) {
        continue;
      }
      if (isDir) {
        this.scanDir(full, driveC, zHome, out);
        continue;
      }
      if (!name.toLowerCase().endsWith('.lnk')) {
        continue;
      }
      const parsed = this.parseLnk(full, driveC, zHome);
      if (parsed) {
        out.push(parsed);
      }
    }
  }

  private parseLnk(path: string, driveC: string, zHome: string): LnkTarget | null {
    let fd = -1;
    try {
      const st = fs.statSync(path);
      if (st.size < 76 || st.size > 1024 * 1024) {
        return null;
      }
      fd = fs.openSync(path, fs.OpenMode.READ_ONLY).fd;
      const buf = new ArrayBuffer(st.size);
      if (fs.readSync(fd, buf, { offset: 0, length: st.size }) < st.size) {
        return null;
      }
      const dv = new DataView(buf);
      // 头: LinkSize==0x4C + LinkCLSID {00021401-…-0046}
      if (dv.getUint32(0, true) != 0x4C) {
        return null;
      }
      const clsid = [0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46];
      const bytes = new Uint8Array(buf);
      for (let i = 0; i < 16; i++) {
        if (bytes[4 + i] != clsid[i]) {
          return null;
        }
      }
      const flags = dv.getUint32(0x14, true);
      const isUnicode = (flags & 0x80) != 0;
      let pos = 76;
      // LinkTargetIDList: 跳过
      if (flags & 0x01) {
        if (pos + 2 > st.size) {
          return null;
        }
        const idListSize = dv.getUint16(pos, true);
        pos += 2 + idListSize;
      }
      // LinkInfo: 取 LocalBasePath(Unicode) (+ CommonPathSuffix 兜底)
      let target = '';
      if (flags & 0x02) {
        if (pos + 0x24 > st.size) {
          return null;
        }
        const liSize = dv.getUint32(pos, true);
        const liHdrSize = dv.getUint32(pos + 4, true);
        const liFlags = dv.getUint32(pos + 8, true);
        if (liSize < 0x1C || pos + liSize > st.size) {
          return null;
        }
        if (liFlags & 0x01) { // VolumeIDAndLocalBasePath
          if (liHdrSize >= 0x24) {
            const uniOff = dv.getUint32(pos + 28, true);
            target = this.readUtf16Z(bytes, pos + uniOff, st.size);
          }
          if (!target) {
            const ansiOff = dv.getUint32(pos + 16, true);
            target = this.readAnsiZ(bytes, pos + ansiOff, st.size);
          }
          // LocalBasePath 通常已含文件名; 少数写入器只写到目录, 拼 CommonPathSuffix
          if (target && !target.toLowerCase().endsWith('.exe')) {
            let suffix = '';
            if (liHdrSize >= 0x24) {
              const suOff = dv.getUint32(pos + 32, true);
              suffix = this.readUtf16Z(bytes, pos + suOff, st.size);
            }
            if (!suffix) {
              const saOff = dv.getUint32(pos + 24, true);
              suffix = this.readAnsiZ(bytes, pos + saOff, st.size);
            }
            if (suffix) {
              target = `${target}\\${suffix}`;
            }
          }
        }
        pos += liSize;
      }
      if (!target) {
        return null;
      }
      // StringData: 只需要 NAME_STRING (HasName); 之前按序跳过
      let displayName = '';
      const stringFlags = [0x04, 0x08, 0x10, 0x20, 0x40];
      for (let i = 0; i < stringFlags.length; i++) {
        const f = stringFlags[i];
        if (!(flags & f)) {
          continue;
        }
        if (pos + 2 > st.size) {
          break;
        }
        const count = dv.getUint16(pos, true);
        pos += 2;
        const byteLen = count * (isUnicode ? 2 : 1);
        if (pos + byteLen > st.size) {
          break;
        }
        if (f == 0x04) {
          displayName = isUnicode
            ? this.readUtf16(bytes, pos, count)
            : this.readAnsi(bytes, pos, count);
        }
        pos += byteLen;
      }
      // Windows 路径 → native
      const native = this.resolveWinPath(target, driveC, zHome);
      if (!native || !native.toLowerCase().endsWith('.exe')) {
        return null;
      }
      const fileName = path.substring(path.lastIndexOf('/') + 1);
      const fallback = fileName.substring(0, fileName.length - 4);
      return { name: displayName || fallback, target: native };
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'parse fail %{public}s: %{public}s', path, JSON.stringify(e));
      return null;
    } finally {
      if (fd >= 0) {
        fs.closeSync(fd);
      }
    }
  }

  private readUtf16Z(bytes: Uint8Array, off: number, size: number): string {
    let s = '';
    for (let i = off; i + 1 < size; i += 2) {
      const c = bytes[i] | (bytes[i + 1] << 8);
      if (c == 0) {
        break;
      }
      s += String.fromCharCode(c);
    }
    return s;
  }

  private readUtf16(bytes: Uint8Array, off: number, count: number): string {
    let s = '';
    for (let i = 0; i < count; i++) {
      s += String.fromCharCode(bytes[off + i * 2] | (bytes[off + i * 2 + 1] << 8));
    }
    return s;
  }

  private readAnsiZ(bytes: Uint8Array, off: number, size: number): string {
    let s = '';
    for (let i = off; i < size; i++) {
      if (bytes[i] == 0) {
        break;
      }
      s += String.fromCharCode(bytes[i]);
    }
    return s;
  }

  private readAnsi(bytes: Uint8Array, off: number, count: number): string {
    let s = '';
    for (let i = 0; i < count; i++) {
      s += String.fromCharCode(bytes[off + i]);
    }
    return s;
  }

  // "C:\foo\bar.exe" → drive_c/foo/bar.exe; "Z:\a\b.exe" → zHome/a/b.exe。
  // 逐段大小写不敏感: 每段在父目录 listFile 里找 lowercase 匹配项。
  private resolveWinPath(winPath: string, driveC: string, zHome: string): string {
    const m = winPath.match(/^([A-Za-z]):[\\\/](.*)$/);
    if (!m) {
      return '';
    }
    const drive = m[1].toUpperCase();
    let base = '';
    if (drive == 'C') {
      base = driveC;
    } else if (drive == 'Z') {
      base = zHome.endsWith('/') ? zHome.substring(0, zHome.length - 1) : zHome;
    } else {
      return '';
    }
    const segs = m[2].split(/[\\\/]+/);
    let cur = base;
    for (const seg of segs) {
      if (!seg) {
        continue;
      }
      let found = '';
      try {
        const names = fs.listFileSync(cur);
        const lower = seg.toLowerCase();
        for (const n of names) {
          if (n.toLowerCase() == lower) {
            found = n;
            break;
          }
        }
      } catch (e) {
        return '';
      }
      if (!found) {
        return '';
      }
      cur = `${cur}/${found}`;
    }
    // 确认最终是已存在的文件
    try {
      return fs.statSync(cur).isFile() ? cur : '';
    } catch (e) {
      return '';
    }
  }
}
