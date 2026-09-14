// 可启动文件判据 (文件管理 / 应用库 / 快捷方式扫描共用)。
//
// 两档类型:
// - 程序文件: Wine 可直接 CreateProcess 的类型。.exe 是 PE 映像; .bat/.cmd
//   由 kernelbase 转 cmd.exe 执行 (dlls/kernelbase/process.c create_cmd_process)。
//   快捷方式扫描的目标、下载目录扫描都收这一档。
// - 可启动文件: 程序文件 + .lnk。.lnk 是壳快捷方式, 不能 CreateProcess,
//   需经 start.exe 走 ShellExecute 由 shell32 解析 (见 AppLibraryService.launch)。
//   文件管理里"启动"按钮、导入自动入库按这一档判断。
const PROGRAM_SUFFIXES: string[] = ['.exe', '.bat', '.cmd'];

const LAUNCHABLE_SUFFIXES: string[] = ['.exe', '.bat', '.cmd', '.lnk'];

function hasAnySuffix(name: string, suffixes: string[]): boolean {
  const lower = name.toLowerCase();
  for (const s of suffixes) {
    if (lower.endsWith(s)) {
      return true;
    }
  }
  return false;
}

export function isProgramFile(name: string): boolean {
  return hasAnySuffix(name, PROGRAM_SUFFIXES);
}

export function isLaunchableFile(name: string): boolean {
  return hasAnySuffix(name, LAUNCHABLE_SUFFIXES);
}

// 显示名: 去掉可启动后缀 ("foo.exe" → "foo"); 无后缀则原名
export function displayNameOf(fileName: string): string {
  const lower = fileName.toLowerCase();
  for (const s of LAUNCHABLE_SUFFIXES) {
    if (lower.endsWith(s)) {
      return fileName.substring(0, fileName.length - s.length);
    }
  }
  return fileName;
}
