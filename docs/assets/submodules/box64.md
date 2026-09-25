# box64 补丁清单

> 基线：8f445d9a（2026-06-12，upstream/main merge-base，winehua 独有 15 commit）
> 生成：2026-08-01，统计更新：2026-09-22
> 说明：box64 在鸿蒙上的角色是 x86 指令翻译器（LIBBOX64_SO 共享库模式，由 wine_child 进程 dlopen）。清单服务未来合并 ptitSeb/box64 main 时确认每个 hunk 的意图与不变式

## 变更总览

- **修改 28 文件 / 新增 10 文件**（+3775 / -1172 行）
- **新增文件（10）**：LIBBOX64_SO 入口 `box64_lib.c`；musl 缺失 libc 移植 `musl_fts.c`(+1255)、`musl_obstack.c`(+378)、`musl_compat.c`(+524)、`musl_error.c`、`musl_box32_stubs.c`(+73) 及配套头 `include/fts.h`、`include/obstack.h`、`include/error.h`、`include/config.h`
- **修改文件分类**：
  - OHOS 内核/沙箱限制（mmap RWX、noexec、无 execve）：`os/os_linux.c`、`custommem.c`、`custommmap.c`、`tools/bridge.c`、`main.c`
  - LIBBOX64_SO 共享库支持：`CMakeLists.txt`、`box64context.c`、`core.c`、`box64_lib.c`
  - glibc 私有符号/musl 布局差异：`wrappedlibc.c`、`wrappedlibc_private.h`、`wrappedlibdl.c`、`libtools/*`、`include/myalign*.h`、`wrapped32/*`
  - BOX32（32 位 x86 模拟）低 4GB 堆：`wrapped32/wrappedlibc.c`、`musl_compat.c`、`musl_box32_stubs.c`
  - 其他：`mallochook.c`（整文件重写）、`dynarec_native_pass.c`（注释残留）

## 变更明细

### A. musl 缺失 libc 功能移植（新增文件，合并上游冲突敏感区）

#### src/musl_fts.c + src/include/fts.h：fts(3)
- **为什么存在**：OHOS musl 无 `fts_open/fts_read/fts_close` 系列，guest 程序（如依赖它的 wine 组件/库）调用会链接失败或符号缺失。直接从 NetBSD 移植 fts 实现（`$NetBSD: fts.c,v 1.48` 2015）。
- **依赖的上游行为**：无——纯第三方代码，与 box64 主树无交互；`include/config.h` 是手写替代 autotools configure 输出的最小配置。
- **不变式**：`config.h` 的 `HAVE_DIRENT_D_TYPE` 等宏与 OHOS musl 实际能力一致，否则 fts 行为（d_type 读取路径）会错。
- **验证方法**：构建 + 运行调用 fts 的 guest 程序；`make NATIVE_ARCH=arm64-v8a` 编译通过。

#### src/musl_obstack.c + src/include/obstack.h：obstack
- **为什么存在**：OHOS musl 无 obstack（glibc 私有）。移植 glibc 的 obstack.c/h（`_OBSTACK_INTERFACE_VERSION` 机制保留）。
- **依赖的上游行为**：glibc obstack 的二进制接口（`obstack_grow` 宏与 `_obstack_*` 内部函数配对）；`myalign32.c` 中 guest obstack 结构转换依赖 glibc 布局。
- **不变式**：`include/obstack.h` 与 `musl_obstack.c` 必须同版本配套；musl 的 `struct obstack` 布局与 glibc 不同（union 成员不同），`myalign32.c` 已用 `#ifndef __MUSL__` 跳过 `tempptr/chunkfun/freefun` 转换。
- **验证方法**：运行使用 obstack 的 guest 程序；BOX32 下运行 32 位 guest 验证 `convert_obstack_to_32/64`。

#### src/musl_compat.c：glibc 私有符号 stub 集合（524 行）
- **为什么存在**：OHOS musl 缺一批 glibc 符号，box64 自身及 guest 链接需要：`__libc_malloc/free/calloc/realloc/memalign/valloc/pvalloc`、pthread NP 亲和性/robust/prioceiling、`dlinfo`、`qsort_r`（musl 版签名不同，用 TLS trampoline 转 `qsort`）、`glob64/globfree64`、`scandirat/scandirat64`、`obstack_printf/vprintf`、`__ctype_b_loc/tolower/toupper`（构造 384 项表，返回 +128 偏移指针，模拟 glibc [-128,255] 索引）、`isnanf/isinff/finitef/exp10*`。全部 `__attribute__((weak))`。
- **依赖的上游行为**：这些符号在 glibc 平台由 libc 提供，box64 上游代码直接引用/声明而无需关心。
- **不变式**：weak 符号使 NDK 未来补上真实现时静默覆盖；`__ctype_*_loc` 表项标志位须与 glibc 位定义一致（guest 按 glibc 位掩码检查）。
- **验证方法**：构建 + 启动 wine（wineboot 全流程）；对调用 `qsort_r/dlinfo` 的 guest 程序回归。

#### src/musl_error.c + src/include/error.h：GNU error(3)
- **为什么存在**：OHOS musl 无 `<error.h>`；移植为 inline 实现 + `musl_error.c` 提供数据符号（`error_message_count` 等）。
- **依赖的上游行为**：glibc error(3) 的输出语义。
- **不变式**：数据符号定义不能缺（否则链接错）；文件在 CMake 中已挂到 box64 与 box64_hmos_core 两个目标。
- **验证方法**：编译通过即可；错误路径触发时观察 stderr 输出格式。

#### src/musl_box32_stubs.c：BOX32 链接 stub
- **为什么存在**：BOX32 构建在 OHOS musl 上链接缺符号：`getprotobyname_r/getprotobynumber_r`（用非 _r 版本包装）、`__res_state`（返回静态变量）、`__chk_fail`（FORTIFY 失败 abort）、`getcontext/makecontext/swapcontext`（ucontext，wrappedexpat XML 协程路径引用，musl 无实现，返回 -1）。commit 414e231cd 给 pc 模式可执行目标也补齐（与 libbox64.so 一致）。
- **依赖的上游行为**：glibc 提供这些符号。
- **不变式**：`__res_state` 返回的静态实例被 `libc_net32.c` 的 `convert_res_state_*` 使用，字段布局按 musl `struct __res_state`（无 `__glibc_unused_*hook`）。
- **验证方法**：`make NATIVE_ARCH=arm64-v8a`（BOX32 开启）链接通过；32 位 guest 启动。ucontext stub 是运行时失败路径，触发即 abort（已知限制）。

### B. OHOS 内核/沙箱限制（mmap/exec）

#### src/os/os_linux.c: InternalMmap（四块修改，必须覆盖）
- **为什么存在**：OHOS 三个内核级限制——①匿名 RWX mmap 直接 EPERM：改为先 RW mmap 再 `mprotect` 加 X（commit e4321658a 进一步跳过"注定失败"的第一次 mmap，避免每次白白一次 syscall）；②noexec 文件系统上文件映射 + PROT_EXEC 失败：EPERM 时改 anon mmap + `pread/read` 把内容读进内存（Wine 从 noexec 分区加载 x86 ELF 的路径）；③`prot & PROT_EXEC` 前需 `prctl(0x6a6974, 0, 0)` 打开内核 JIT 开关（"jit" ASCII）。
- **依赖的上游行为**：上游假设 Linux 内核允许匿名 RWX 且文件映射可带 PROT_EXEC；`#else`（非 STATICBUILD）分支从 `dlsym(RTLD_NEXT,...)` 改为 `RTLD_DEFAULT`（musl 无 RTLD_NEXT 链语义）。
- **不变式**：JIT prctl 必须在任何 PROT_EXEC 映射前调用；RW+mprotect 在映射返回与 mprotect 之间窗口内数据可写；anon+pread fallback 用 `MAP_FIXED` 位保留语义；EPERM 区分要精确（只对 `prot&PROT_EXEC` 且 `fd!=-1` 走 fallback），否则正常文件映射失败会被误吞。
- **验证方法**：完整构建 + wine 启动（wineboot/explorer 走 EXEC 映射路径）；`/proc/self/maps` 观察 JIT 区；hilog 无 EPERM 循环报错。

#### src/os/os_linux.c: PersonalityAddrLimit32Bit
- **为什么存在**：`M_ARENA_TEST/M_ARENA_MAX/M_MMAP_THRESHOLD` 是 glibc mallopt 常量，OHOS musl 无定义，编译失败。
- **依赖的上游行为**：32 位模式下调 malloc arena 数。
- **不变式**：`#ifdef` 守卫后 musl 下 mallopt 调用被跳过（musl 不支持也无意义）。
- **验证方法**：编译通过；BOX32 guest 启动。

#### src/custommem.c: box_mmap（mmap fallback 链 + MAP_32BIT 硬搜索）
- **为什么存在**：①OHOS ARM64 内核**未实现 `MAP_FIXED_NOREPLACE`（ENOSYS）**，且 x86_64 的 `MAP_FIXED_NOREPLACE=0x200000` 在 aarch64 上是别的值——改为 aarch64 值 `0x100000`；②hint mmap 失败后依次重试：NULL 地址重试 → 匿名 RWX 失败改 RW+mprotect；③`box_maps_search_low4gb`（`box_mmap32_hard_search_ohos`）：32 位 guest 需要 <4GB 地址，解析 `/proc/self/maps` 找低 4GB 空洞，64KB 对齐后用 MAP_FIXED 抢占（`MAP_FIXED_NOREPLACE` 不可用时的替代方案）。
- **依赖的上游行为**：上游假设 `MAP_FIXED_NOREPLACE` 可用、`MAP_32BIT` 语义可用；`find31bitBlockNearHint` 在 OHOS 上不可靠（内核忽略低地址 hint）。
- **不变式**：fallback 只在 `addr` 非空且非 `MAP_FIXED` 时用 NULL 重试（MAP_FIXED 语义不能变）；RWX→RW+mprotect 后保护位最终必须等于请求的 prot；硬搜索用 mutex 串行化 + 每次 mmap 后校验返回地址等于请求地址（MAP_FIXED 必须精确命中）。
- **验证方法**：BOX32 guest（32 位 exe）长时间运行观察无 `load_addr_32bits` 越界；32 位程序反复 malloc 无 ENOMEM。

#### src/custommem.c: init_custommem_helper
- **为什么存在**：`dlsym(RTLD_NEXT, "__curbrk")` 在 musl 上无此符号（glibc 私有），改为置 `cur_brk = NULL`。
- **依赖的上游行为**：上游用 `__curbrk` 读取 brk 位置。
- **不变式**：`cur_brk` 为 NULL 时使用方必须走 fallback 路径——[待确认] 需核实引用处是否处理 NULL。
- **验证方法**：wine 全流程启动无崩溃。

#### src/tools/bridge.c: NewBrick
- **为什么存在**：bridge 代码页（JIT 翻译跳板）原一次性申请 RWX，OHOS 匿名 RWX EPERM → 改为 RW mmap 成功后 `mprotect` 加 X；mprotect 失败仅警告不 abort（保留 RW 映射"might still work"）。
- **依赖的上游行为**：`box_mmap` 一次返回 RWX 映射。
- **不变式**：`setProtection_box` 仍标记 RWX+NOPROT；mprotect 失败时 bridge 区不可执行——[待确认] 是否依赖后续 box64 自身的再保护逻辑，失败可能只在部分指令路径出问题。
- **验证方法**：任意 x86 程序执行（每个翻译块都过 bridge）。

#### src/main.c: main
- **为什么存在**：入口即 `syscall(__NR_prctl, 0x6a6974, 0, 0)` 开启 JIT（保证最早生效），对应 wrappedlibc 中 guest 侧 0x6a6974 prctl 的处理。
- **依赖的上游行为**：上游 main 直接进 initialize。
- **不变式**：pc 模式可执行与 .so 模式都必须在首个 PROT_EXEC 映射前开 JIT。
- **验证方法**：pc 模式（arm64 Linux 主机）回归启动。

#### src/custommmap.c: mmap64
- **为什么存在**：`add_32bit` 条件提取为局部变量，避免重复求值并让 OHOS 分支（见 custommem.c fallback）行为一致。
- **依赖的上游行为**：`BOX64ENV(mmap32)` / `MAP_32BIT` / `box64_is32bits` 判定逻辑。
- **不变式**：判定条件等价于上游（仅重构）。
- **验证方法**：BOX32 guest 的 mmap 回归。

### C. LIBBOX64_SO 共享库支持

#### src/box64_lib.c（新增）
- **为什么存在**：ARM64 Pad 无 execve，box64 不能作为独立可执行文件运行。提供 `box64_hmos_main(argc, argv, env)`：调 `initialize` + `emulate`，由宿主 wine_child NCP 子进程 `dlopen("box64.so")` 后调用，同一进程内模拟执行 x86_64 Wine ELF。
- **依赖的上游行为**：`initialize/emulate` 的可重入性——上游 `emulate` 返回后进程退出，.so 场景需宿主继续存活。
- **不变式**：argv[0] 是 "box64" 哨兵而非文件路径（core.c 有专门处理）；返回 0=成功/-1=初始化失败。
- **验证方法**：winehua 完整启动（wineboot → explorer → app）。

#### CMakeLists.txt：`-DLIBBOX64_SO=ON`
- **为什么存在**：新增 `box64_hmos_core` SHARED 目标（OUTPUT_NAME box64 → box64.so），链接 mainobj/interpreter/dynarec/WRAPPERS；ANDROID 下额外链 `libhilog_ndk.z.so`；5 个 musl 源文件挂到 SHARED 目标；`mainobj` 也编译带 `LIBBOX64_SO=1`；另有 5 段 `if(TARGET box64)` 把 musl 源文件挂到 pc 模式可执行目标（414e231cd）。
- **依赖的上游行为**：上游 `BOX64` 可执行目标的链接结构。
- **不变式**：`box64` 与 `box64_hmos_core` 两份 musl 源文件列表必须同步（现有 `if(TARGET box64)` 追加写法保证可执行目标也有）；mainobj 的 LIBBOX64_SO 宏与 SHARED 目标一致。
- **验证方法**：`make NATIVE_ARCH=arm64-v8a` 产物含 box64.so；pc 模式构建回归。

#### src/box64context.c: NewBox64Context（box64lib 获取）
- **为什么存在**：上游 `dlopen(NULL)` 取自身 handle；box64 被 dlopen 进宿主后 `dlopen(NULL)` 返回宿主全局符号表（不含 box64 导出符号）。改为 `dladdr(NewBox64Context)` 找到自身 .so 路径再 dlopen，fallback `dlopen("box64.so")` → `dlopen(NULL)`。非 LIBBOX64_SO 分支直接置 NULL（`OHOS_PATCH_SKIP_BOX64LIB`，musl 下 dlopen(NULL) 无意义）。
- **依赖的上游行为**：`box64lib` 用于在自身符号表里解析（GetNativeSymbolUnversioned 等）。
- **不变式**：LIBBOX64_SO 下 `box64lib` 必须是 box64.so 自身 handle，否则 guest dlsym 解析不到 box64 导出符号。
- **验证方法**：LIBBOX64_SO 构建 + wine 启动（依赖 box64 导出符号的 guest 调用路径）。

#### src/core.c: initialize（3 处）
- **为什么存在**：①`box64path`：argv[0]="box64" 哨兵无法 `ResolveFile`，用 dladdr 取 .so 真实路径；②`IS_FILE|IS_EXECUTABLE` 检查：OHOS noexec 文件系统上 x86 ELF 无 exec 位，LIBBOX64_SO 下只查 `IS_FILE`（commit 4d15bb186）；③argv 内存前移改写（隐藏 "box64" 标记）：box64 被 dlopen 后改写宿主 argv 会崩 appspawn 持有的原始指针（ps/hilog 读取崩溃），改为直接用内部构造的 argv。
- **依赖的上游行为**：上游 `orig_argc/orig_argv` 来自改写后的 argv。
- **不变式**：LIBBOX64_SO 下 `orig_argv` 指向 box64 内部 argv（原样指针，宿主 argv 零改动）；`my_prctl(PR_SET_NAME)` 的 `.exe` 后缀 argv[0] 改写逻辑在此模式下失效（上游行为改变，见 wrappedlibc 条目）。
- **验证方法**：winehua 启动后宿主进程 argv 正常（ps/hilog 不崩）；wine 内进程名检查。

### D. wrappedlibc / wrappedlibdl wrap 修复

#### src/wrapped/wrappedlibc.c: my_prctl
- **为什么存在**：①`PR_SET_NAME`：上游处理后 fallthrough 到 native prctl，但 x86_64 ABI 5 寄存器传参——arg4(r10)/arg5(r8) 携带 guest 残留地址（如 PE 加载基址 0x14002d000），OHOS 内核解引用 → SIGSEGV 杀 rundll32/wineboot。改为直接 return 0（进程名已由 box64 初始化时设过）；②`prctl(0x6a6974)`（OHOS JIT 开关）：guest（Wine `virtual_map_image/mprotect_exec`）调用时清零 arg4/arg5 防内核误解引用。
- **依赖的上游行为**：上游 PR_SET_NAME 的 `.exe` → `orig_argv[0]` 改写（wine 进程名 hack）。
- **不变式**：PR_SET_NAME 不再改 orig_argv（LIBBOX64_SO 下本来就不能改宿主 argv，两者一致）；0x6a6974 必须原样转发 option 与 arg2/arg3。
- **验证方法**：启动 rundll32/wineboot 类进程不 SIGSEGV；hilog 无 prctl 崩溃。

#### src/wrapped/wrappedlibc.c: rint/rintf wrap（commit 23750c922）
- **为什么存在**：为 OHOS 上的 guest Mesa 提供 `rint/rintf` wrap——`functions_list.txt` 新增 `fFf: rintf`、`dFd: rint` 强 wrap 类型 + `wrappedlibc_private.h` 启用 `GOWM(rint, dFEd)/GOWM(rintf, fFEf)`（上游此两行被注释）。[待确认] 具体触发症状：疑为 guest 调 rint 时符号解析/版本不匹配导致；OHOS musl 的 rint 是强符号但 box64 wrap 表无此条目时按普通转发处理失败。
- **依赖的上游行为**：上游刻意不 wrap rint（`// GOWM(rint, dFEd)` 注释态，可能因浮点返回 ABI 特殊）。
- **不变式**：dFEd/fFEf（double,float → double）转发路径与 guest 浮点返回 ABI 匹配；generated 头（wrappedlibctypes.h 的 fFf_t/dFd_t）与 functions_list 同步。
- **验证方法**：运行调用 rint 的 guest（DXVK/Mesa 渲染路径）回归。

#### src/wrapped/wrappedlibc.c: PRE_INIT（OHOS_PATCH_NO_PRE_INIT_DLOPEN）
- **为什么存在**：`PRE_INIT` 宏原为 `dlopen(NULL, RTLD_LAZY|RTLD_GLOBAL)`（拿自身 handle 供 wrapped lib 查找符号）；OHOS musl 上 dlopen(NULL) 拿不到 box64 符号 → 空宏（patch 19 注释说明）。
- **依赖的上游行为**：wrap 初始化模板用 `PRE_INIT` 后接 `{ ... }` 复合语句。
- **不变式**：宏为空时模板仍编译（注释确认无尾随分号）；wrapped lib 符号解析改由 libtools/libdl.c 的 RTLD_DEFAULT 路径负责。
- **验证方法**：guest 加载 dlsym 类调用回归；编译通过。

#### src/wrapped/wrappedlibc.c 其他
- `getGlibcCachedTid`：`lock.__data.__owner` 是 glibc pthread 内部字段，musl 无 → 恒返回 0。**不变式**：调用方需容忍 0（[待确认] 影响面：仅调试用途）。
- `__ctype_b_loc/__ctype_tolower_loc/__ctype_toupper_loc` extern 声明 + `_GNU_SOURCE/sched.h` include（与 musl_compat.c 的实现配对）。
- `PTHREAD_RECURSIVE/ERRORCHECK_MUTEX_INITIALIZER_NP` fallback 宏（musl 无 NP 变体）。
- `OHOS_UNDEF_BEFORE_CB/REDEF_AFTER_CB`：musl 下 `stat64` 等 64 位别名宏已不存在（off64_t==off_t），wrapper 生成前后需 undef/redefine 保持 box64 内部符号一致。

#### src/wrapped/wrappedlibdl.c + src/libtools/libdl.c
- **为什么存在**：musl 无 `RTLD_DL_LINKMAP/RTLD_DL_SYMENT` 常量 → 补 `#ifndef` 定义；`RTLD_NEXT` 语义在 musl dlopen 链不同 → 注释/改用 RTLD_DEFAULT；`libdl.c` 的 `dlopen/dlclose/dlsym` EXPORT shim 改名 `box64_unused_*` 并降级为 static——box64 以 .so 加载时导出同名强符号会劫持宿主/guest 的 dl 调用。[待确认] 原 EXPORT 拦截在 LIBBOX64_SO 下由 wrappedlibdl 的 `my_dlsym_internal` 等取代。
- **依赖的上游行为**：上游 `dlopen` shim 拦截 guest dlopen 并把 x86 ELF 交给 emulator。
- **不变式**：删除的 `___dlsym` alias 一并移除；改名后无 EXPORT dl 符号残留。
- **验证方法**：guest dlopen/dlclose 全流程（wine 加载 DLL）回归。

#### src/libtools/threads.c / threads32.c
- **为什么存在**：①`_pthread_cleanup_push/pop` 手写声明删除——musl `<pthread.h>` 已声明，box64 声明冲突；②`init_pthread_helper` 中 `dlsym(NULL, "_pthread_cleanup_push_defer")` 等 + `dlvsym` GLIBC_2.x 版本符号查找全跳过（musl 无版本符号，dlsym(NULL) 无全局表）→ 三个 real_ 指针置 NULL，`real_phtread_kill_old` 直接 `pthread_kill`；③`my_pthread_kill_old` 的 pthread_t 转换加显式 cast（musl pthread_t 是指针）。
- **依赖的上游行为**：glibc 的 `_pthread_cleanup_*` 隐藏符号与 GLIBC 版本化 `pthread_kill`。
- **不变式**：real_ 指针为 NULL 时调用方分支必须安全（上游代码已有 NULL 检查则无碍，[待确认] `real_pthread_cleanup_push_defer` 的使用点是否都有 NULL 守卫）。
- **验证方法**：wine 多线程 guest 程序（含线程退出/清理）回归。

### E. BOX32 支持

#### src/musl_compat.c: OHOS_PATCH_BOX32_LOW4GB_V2（box32 低 4GB heap）
- **为什么存在**：32 位 guest 指针必须 <4GB。实现固定 256MB 堆（0x10000000 起，`MAP_FIXED` 探测成功则精确占位，失败则 hint 步进搜索），bump allocator + 16 字节对齐 + canary（0xB032B032）头，提供 `box32_malloc/calloc/realloc/free/memalign/strdup/malloc_usable_size` 7 个入口。替代原 mallochook.c 中的 `customMalloc32/customCalloc32` 路径。
- **依赖的上游行为**：上游 box32 分配走 mallochook 的 `box32_*`（FITS_IN_32BIT 判定 + customMem32 fallback）。
- **不变式**：堆内指针永远 < 4GB；free 只能回退最后分配块（bump 语义，非 LIFO 释放可导致空间碎片/耗尽）；canary 校验失败 abort 式打印；`MAP_FIXED` 失败必须回退 hint 搜索，不能 abort。
- **验证方法**：32 位 exe 完整运行（explorer/32 位 app）；长跑观察堆耗尽日志（256MB 上限是已知限制）。

#### src/wrapped32/wrappedlibc.c：my32_ 分配族代理
- **为什么存在**：`my32_malloc` 原实现 `calloc(1,size)`（注释"x86 malloc 疑似清零"的 hack）→ 改 `box32_malloc + memset`；`free/calloc/realloc/memalign/posix_memalign/valloc/strdup/strndup/malloc_usable_size` 全部新增 my32_ 代理走 box32 低 4GB 堆；`wrappedlibc_private.h` 对应条目 `GOW→GOWM ... //%%,noE`（不再解析原生符号，直接进代理）。
- **依赖的上游行为**：上游这些函数直接转发宿主 libc。
- **不变式**：所有 guest 分配/释放必须成对走 box32 堆（混用宿主 free 会 canary 失败）；`my32_posix_memalign` 返回 EINVAL/ENOMEM 语义正确。
- **验证方法**：32 位 guest 内存密集程序；valgrind 不可用场景下靠 canary 报错定位。

#### src/wrapped32/wrappedlibc.c 结构差异
- `glob_t` 无 `gl_flags` 字段、`posix_spawn_file_actions_t` 无 `__used/__allocated`（musl 布局）→ 转换函数跳过；`myalign32.h` 补 `__uid_t/__gid_t/__pid_t/__fsblkcnt64_t/__fsfilcnt64_t` typedef（glibc 私有名）。

#### src/musl_box32_stubs.c
- 见 A 节。pc 模式可执行（414e231cd）与 .so 双目标都要链接。

### F. 结构布局 musl 差异（libtools/头文件）

#### src/libtools/signal32.c、src/include/myalign.h、myalign32.h
- **为什么存在**：`siginfo_t::_sifields` → musl `__si_fields`（宏别名）；`__jmp_buf_tag.__saved_mask` 类型 `__sigset_t` → `sigset_t`；musl 无 `__SI_SIGFAULT_ADDL` 扩展字段（空宏）；补 `__pid_t/__uid_t/__gid_t`。
- **依赖的上游行为**：glibc 类型名与 siginfo 布局。
- **不变式**：宏别名在 `#include <signal.h>` 之后定义才生效（当前顺序正确）；若 musl 未来字段布局变化需复查。
- **验证方法**：信号路径回归（guest 收到 SIGSEGV 等时 siginfo 转换正确）。

#### src/libtools/libc_net32.c: convert_res_state
- **为什么存在**：musl `struct __res_state` 无 `__glibc_unused_qhook/rhook`（glibc 私有）→ 转换跳过。
- **不变式**：`__res_state`（musl_box32_stubs.c 提供）返回的实例字段与转换函数假定一致。
- **验证方法**：guest 做 DNS 解析（getaddrinfo 路径）回归。

### G. src/mallochook.c：整文件重写（1101 → 71 行）

- **为什么存在**：原文件是 "Exterminate" 策略的全局 malloc 拦截器（内部内存池 + spin lock + 强符号覆盖 `malloc/free/calloc/realloc` + TBB/tcmalloc 兼容 + box32 低 4GB 判定）。**怀疑其在 OHOS musl 的 ctor 阶段死循环**（文件头注释原文），故整体替换为诊断 passthrough：`box_malloc/free/calloc/realloc/memalign/strdup/strndup` 直接转 libc，`box_free_internal` 直接 free，`box_malloc_usable_size` 返回 0，`init/start/endMallocHook` 空操作，`box_realpath` 保留，不再强符号覆盖系统 malloc（注释："让 musl 自己处理"）。
- **依赖的上游行为**：上游 box64 各模块依赖 `box_*`/`box32_*` API；`box32_*` 已由 musl_compat.c 低 4GB 堆接管；mallochook 的符号覆盖能力（tcmalloc/malloc_hack_2）全部丢失。
- **不变式**：所有调用点使用 `box_*` 符号而非直接 malloc（保持 API 面）；`box_malloc_usable_size` 返回 0 的调用方必须容忍；guest 强覆盖 malloc（tcmalloc 类）会绕过 box64——已知能力损失。
- **验证方法**：winehua 完整启动（此文件是启动期死循环修复的关键，启动成功即验证）；注意注释自述"仅用于定位启动期死循环，不可用于真正翻译 x86 程序"——[待确认] 当前生产构建是否仍使用此 passthrough 版本（若是，guest 侧 tcmalloc 场景属已知风险）。

### H. 日志与诊断清理

#### src/dynarec/dynarec_native_pass.c
- **为什么存在**：仅留注释 `OHOS-DIAG: 32-bit block tracking disabled for production`（commit 799b2febb 清理诊断日志后残留）。
- **不变式**：无（纯注释）。
- **验证方法**：编译通过。

#### 其他清理（无行为变化）
- `my_reserveHighMem` 的 reserve 日志注释掉、`internal_customMemAligned` 的 `if(1 || ...)` 去恒真、`box64_wine || 1` → `box64_wine`（47bit 限制仅 wine 模式）、`custommem.c` 空 if 块移除、wrappedlibc 残留 `stdio.h` include 移除、libdl.c 末尾无换行修复。均不改变语义，合并时可直接丢弃。

## 合并上游注意点（摘要）

1. **musl_* 系列新文件是纯第三方移植（NetBSD/glibc）**，与 box64 主树零交互，但 CMakeLists 的 5 段 `if(TARGET box64)` 追加是 winehua 特有接线，与上游 CMake 结构冲突敏感。
2. **mallochook.c 是最激进的重写**：上游该文件持续演进（tcmalloc 兼容、malloc_hack 2），winehua 的 passthrough 与上游任何修改都会冲突——合并时应评估是否可把 OHOS 修复收敛为上游 `init_malloc_hook` 的条件分支（`#ifdef __OHOS__`），而非整体替换。
3. **os_linux.c InternalMmap 是行为改动最多处**：RWX→RW+mprotect、JIT prctl、noexec pread fallback 三者均为 `#ifdef __OHOS__` 或独立可移植逻辑，可保留为上游特性；JIT prctl(0x6a6974) 为 OHOS 专有。
4. **wrappedlibc 的 prctl(PR_SET_NAME) 直接 return 0** 改变了上游 wine 进程名 hack 行为，仅 LIBBOX64_SO 场景必须；pc 模式建议保留上游逻辑。
5. **BOX32 低 4GB heap** 依赖 OHOS 内核低地址 hint 精确性（注释明确 "ARM64 kernel honours low address hints precisely"），移植到其他平台需重验。
6. 多数 wrap 修改（`__ctype_*_loc`、NP 宏 fallback、RTLD_DEFAULT、glibc 私有字段跳过）本质是 **glibc→musl 可移植性修复**，值得反哺上游作为 musl 支持；`#ifdef __OHOS__` 与 `#ifndef __MUSL__` 的标记粒度已为合并做好准备。
