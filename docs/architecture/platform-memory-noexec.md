# Wine mmap + PROT_EXEC + fd 深度分析

> 更新: 2026-06-12（2026-07-31 标注落地状态）
> 主题: 在 noexec 文件系统上 mmap(MAP_PRIVATE, fd) + mprotect(PROT_EXEC) 失败的根本原因
> 状态: ✅ **已解决**（CURRENT_STATUS 已修复问题 #2）——以下"修复方案"为当时设计，
> 现行代码已演进为 `ohos_map_exec_section()` / `ohos_mprotect_exec()`（见文末更新）

---

## 一、核心问题

HNP 安装路径 `/data/service/hnp/` 挂载为 **noexec**，Wine 加载 PE DLL 时无法为代码段添加 PROT_EXEC：

```
map_image_into_view failed to set ... protection on ... section .text, noexec filesystem?
```

Linux 内核在 `mprotect` 添加 PROT_EXEC 时检查文件系统是否禁止可执行映射 (`MS_NOEXEC`)。即使 mmap 使用 `MAP_PRIVATE`，内核也会拒绝。

---

## 二、PE 加载全流程

### 调用链

```
virtual_map_builtin_module()
  → virtual_map_image()
    → map_image_view()                    # 分配匿名视图，strip PROT_EXEC
    → map_image_into_view()               # 填充 PE 内容
      → map_pe_header()                   # PE 头映射
      → for each section:
        → map_file_into_view()            # mmap(fd) → 文件支持页 (noexec 下失败)
      → set_vprot()                       # mprotect 加 PROT_EXEC → ❌ 拒绝
```

### 两条路径对比

| 路径 | 页面类型 | 后续 mprotect(PROT_EXEC) | 文件系统要求 |
|------|---------|-------------------------|-------------|
| mmap(fd) 成功 | 文件支持 | ❌ 失败 (noexec) | 需 fs 无 noexec |
| pread 回退 | 匿名 | ✅ 成功 | 无限制 |

---

## 三、修复方案（当时设计，已被现行实现取代）

在 `map_image_into_view()` 中，可执行段 (`IMAGE_SCN_MEM_EXECUTE`) 直接用匿名 mmap + pread：

```c
if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
{
    // 匿名 mmap 替代文件支持映射
    mmap(sec_addr, sec_map_size, PROT_READ | PROT_WRITE,
         MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
    pread(fd, sec_addr, file_size, file_start);
    // 后续 set_vprot → mprotect(PROT_EXEC) → ✅ 成功
}
```

### 为什么 prctl 绕过不够

`prctl(0x6a6974)` 对匿名页面有效，但对**文件支持**页面（有 backing store），内核仍然拒绝 PROT_EXEC。必须先让页面匿名化。

### `mprotect_exec()` 辅助（当时方案）

```c
static inline int mprotect_exec(void *base, size_t size, int unix_prot)
{
    if (unix_prot & PROT_EXEC)
    {
        prctl(0x6a6974, 0, 0);     // 暂时允许 exec
        int ret = mprotect(base, size, unix_prot);
        prctl(0x6a6974, 0, 1);     // 恢复 noexec
        return ret;
    }
    return mprotect(base, size, unix_prot);
}
```

> ⚠️ prctl(0x6a6974) 方案已废弃：该 prctl 本身在 Box64 下会 SIGSEGV（CURRENT_STATUS
> 已修复问题 #3），且 2026-07-28 恢复了 executable PE section protection（c31c2a3）。

### 现行实现（2026-07）

- `dlls/ntdll/unix/ohos_virtual.c` — `ohos_map_exec_section()`（替代内嵌的匿名 mmap + pread
  逻辑，内部 `ohos_jit_enable()` 配对）与 `ohos_mprotect_exec()`（委托 JIT enable + mprotect）
- `dlls/ntdll/unix/virtual.c:1950` 附近 — `IMAGE_SCN_MEM_EXECUTE` 分支调用 `ohos_map_exec_section()`
- 核心结论不变：noexec 下文件映射 + PROT_EXEC 必须匿名化，这是现行代码的设计依据

---

## 四、修复覆盖范围

| 场景 | 覆盖? | 说明 |
|------|-------|------|
| `map_image_into_view` 可执行段 | ✅ | 匿名 mmap + pread |
| 非可执行段 | N/A | 不需要 PROT_EXEC |
| PE header 映射 | N/A | VPROT_READ 无 EXEC |
| `virtual_create_builtin_view` | N/A | ELF loader 管理 |
| 普通 NtAllocateVirtualMemory + EXEC | ✅ | 匿名 mmap → mprotect_exec |

---

## 五、源码位置索引

| 文件 | 关键函数 |
|------|---------|
| `dlls/ntdll/unix/ohos_virtual.c` | `ohos_map_exec_section` / `ohos_mprotect_exec` / `ohos_jit_enable` (现行) |
| `dlls/ntdll/unix/virtual.c` | `map_image_into_view` (OHOS 匿名 mmap fix) |
| `dlls/ntdll/unix/virtual.c` | `map_view`, `map_file_into_view`, `map_pe_header` |
| `server/mapping.c` | `check_current_dir_for_exec` |
