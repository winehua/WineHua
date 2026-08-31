#ifndef WINEHUA_ENV_SPEC_H
#define WINEHUA_ENV_SPEC_H

/**
 * env_spec.h — 结构化环境变量 (EnvSpec)
 *
 * 背景: OHOS NCP 子进程不继承主进程 environ, wine 子进程环境唯一权威通道是
 * entryParams 尾部内嵌的 "|__env=K=V" 段 (NCP 直启与 broker SPAWN 同一格式,
 * 见 docs/PROCESS_SPAWNING.md)。该文本通道有两条硬规则, 此前散落在多个
 * 序列化点各自实现, 此处收口为全项目唯一实现:
 *
 *   1. 不可编码: entryParams 以 '|' 分段、按 '\n' 行解析, 键或值含 '|'/'\n'
 *      的条目无法安全编码, 序列化时丢弃 (见 IsEntryParamsEncodable)。
 *   2. fd 变量禁入: fd 号跨进程失效, per-process fd 变量绝不进文本通道,
 *      由 NCP fdList / SCM_RIGHTS 传 fd, 子进程按本进程 fd 号重写变量
 *      (wine_child.cpp), 见 IsPerProcessFdEnvKey。
 *
 * 镜像实现: thirdparty/wine/dlls/ntdll/unix/ohos_broker.c env_forwardable()
 * 遵守同一契约 (wine 构建系统独立编译, 无法 include 本头; 改一侧必须同步另一侧)。
 */

#include <string>
#include <utility>
#include <vector>

namespace winehua {

// fd 变量: WINESERVERSOCKET / WINE_OHOS_AUDIO_* 四项, 跨进程按 fd 传递
bool IsPerProcessFdEnvKey(const std::string& key);

// "K=V" 整行可安全嵌入 entryParams ('|' 分段、'\n' 行界均不得出现)
bool IsEntryParamsEncodable(const std::string& line);

// 有序 K=V 集合: key 唯一, 保首次插入位置, 后写值生效 (与链式
// setenv(...,1) / UpsertEnvLine 的"后者胜出"语义一致)
class EnvSpec {
public:
    void set(const std::string& key, const std::string& value);
    // 解析 "K=V" 行后 upsert; 无 '=' 或空 key 的非法行忽略
    void setLine(const std::string& line);
    // other 的条目逐项 upsert 进来 (other 胜出)
    void mergeFrom(const EnvSpec& other);

    bool has(const std::string& key) const;
    const std::string* get(const std::string& key) const;  // nullptr = 不存在
    size_t size() const { return entries_.size(); }
    const std::vector<std::pair<std::string, std::string>>& entries() const { return entries_; }

    // 序列化为 "|__env=K=V|__env=K2=V2..."; 自动跳过 fd 变量与不可编码条目
    std::string serializeEntryParams() const;

    // 迁移期与旧 vector<string> 表示互操作; fromLines 对同 key 行取最后值
    static EnvSpec fromLines(const std::vector<std::string>& lines);
    std::vector<std::string> toLines() const;

private:
    std::vector<std::pair<std::string, std::string>> entries_;
    long indexOf(const std::string& key) const;  // -1 = 不存在 (线性扫描, 条目量级 <100)
};

} // namespace winehua

#endif // WINEHUA_ENV_SPEC_H
