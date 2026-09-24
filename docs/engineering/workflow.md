# 协作流程

> 适用场景：开分支、提交代码、处理 submodule 改动、合并别人的改动之前。
> 最后核实：2026-09-24
> 相关文档：[quality.md](quality.md)（改完要跑什么）、[coding.md](coding.md)（代码规范）、[../assets/submodule-maintainability.md](../assets/submodule-maintainability.md)（各 fork 的现状与合并策略）

## 两条长期分支

| 分支 | 定位 | 版本号 |
|---|---|---|
| `master` | 公共技术线：Wine、合成器、图形栈、构建 | 1.0.13 |
| `main-ui` | 产品线与发布线：界面功能、平板多窗口、应用库；发布包从这里出 | 1.0.31 |

两条分支都有各自独立的提交（`main-ui` 上比 `master` 多出一百多个），同时又在持续互相同步。所以「改动提交到哪儿」是有讲究的。

### 两边都要的改动，只在 master 提交一次

改在 `master`，然后 `git checkout main-ui && git merge master` 带过去。不要在 `main-ui` 上再提交一份等价的改动。

**为什么**：两边各提交一份，merge 的时候 Git 会按三方合并的规则把同一段代码合出**重复的两份**——同一个函数调用和它的注释会连着出现两次。

### main-ui 独有的代码，直接在 main-ui 提交

平板多窗口、应用库这类代码，`master` 上根本没有，不存在同步问题，直接在 `main-ui` 上改、提交。

判断某段代码属于哪一类：

```bash
git cat-file -e master:entry/src/main/ets/components/AppLibraryView.ets
# 命令报错 → master 上没有这个文件 → 属于 main-ui 独有
```

已经提交到 `main-ui`、但 `master` 也需要的时候，不要手工再提交一遍，按这个顺序处理：`git checkout master`，把改动应用过去、提交、推送，再切回 `main-ui` 执行 merge——此时是单边改动，不会产生重复。

### 部署前先确认分支

**设备上装的是哪个分支的构建，就在哪个分支构建。** 两个分支的版本号不同，跨分支安装会被系统按「版本降级」拒绝（`error: install version downgrade`），一轮构建白费，而且安装脚本里往往已经清掉了应用数据。

版本号在 `AppScope/app.json5`，不在 `entry/src/main/module.json5`。

## 功能分支

日常开发走 `feature/<名字>`，主仓库和需要改的 submodule 用**同名**分支，方便对照。

```
master ──┬── feature/xxx ──┬── 合并回 master
         │                 └── thirdparty/wine 里的 feature/xxx
```

改完自测、推送、提 PR，主线由维护者合并。

### 有提交就推送

**本地提交要及时推到远程，不要只留在本机。** 未推送的提交只存在于本地仓库，磁盘故障、误删分支、清理目录都会让它彻底消失，没有恢复手段。

## 提交

提交信息用 `类型(范围): 描述` 的格式，描述用中文：

```
fix(build): VirGL 产物过期时不重建
feat(env): 默认渲染策略从 vkd3d_limited_500k 改为 dxvk_modern_2_6
docs: 目录重组与归档整理
submodule: wine → feature/window-resize (窗口可调整性判据)
```

- 常用类型：`feat` `fix` `docs` `build` `perf` `refactor` `chore`。
- 范围写模块名（`build`、`compositor`、`env`……），不是必须写。
- **提交信息里不写协作者署名。**
- 一次提交只做一件事。调试用的临时日志要单独提交（方便整体回退），不要混在功能提交里。

## Submodule 的改动怎么走

这一节是完整的操作流程，照做即可。

### 开发者

```bash
# 1. submodule 里开同名分支、提交、推送
cd thirdparty/<名字>
git checkout -b feature/<名字>
git add . && git commit -m "feat: xxx"
git push origin feature/<名字>

# 2. 回主仓库登记指针（此时指向 feature/<名字>）
cd ../..
git add thirdparty/<名字>
git commit -m "submodule: <名字> → feature/<名字>"
```

推送分支前先跑一遍 `./scripts/check-submodules.sh`，确认 submodule 的提交确实推到了远程。**submodule 里的提交如果没有推送到它自己的远程，主仓库里的指针就是一个指向不存在对象的空指针**，别人拉下来直接构建失败。

### 维护者

审查别人的 PR 时，逐个检查有变更的 submodule：

| 检查什么 | 命令 | 不通过怎么办 |
|---|---|---|
| 提交推到远程了吗 | `cd thirdparty/<名字> && git ls-remote origin $(git rev-parse HEAD)` | 没有输出 → 打回 |
| 提交在默认分支上吗 | `git branch -r --contains HEAD origin/<默认分支>` | 没有输出 → 由维护者把 feature 分支合入默认分支 |
| 主仓库指针指回默认分支了吗 | `./scripts/check-submodules.sh` | 维护者更新指针后重新提交 |

合并顺序：先把 submodule 的改动合进它自己的默认分支，再把主仓库的指针指回默认分支，最后合主仓库的分支。

## 合并之前

合入主线前，除了代码本身，还要过这几项：

1. **构建通过**——按改动范围选构建命令（见 [quality.md](quality.md)）。
2. **相关测试通过**——按 [quality.md](quality.md) 的对照表跑。
3. **submodule 检查通过**——上面那张表。
4. **能快进合并**——`git merge feature/<名字> --ff-only`；不满足说明分支落后于主线，先同步再合。

## 和 AI 助手协作

这个项目的开发过程里有 AI 助手参与，有几条约定：

- **不让 AI 助手往提交信息里加署名。**
- **不让 AI 助手用命令改文件**（`sed`、`echo >` 之类），用编辑工具改——命令改文件出错时没有提示，事后很难发现。
- **不让 AI 助手擅自拷贝构建产物到项目根目录**。产物留在构建目录里，报告路径就够了。
- **涉及 submodule 的操作，先确认当前在哪个目录**。项目里同一个相对路径在主仓库和在 submodule 里指向的是两个不同的地方，切错目录的后果是改错仓库或者提交到错误的分支。
