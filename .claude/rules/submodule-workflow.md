# 分支与 submodule

两条长期分支：`master`（公共技术线）、`main-ui`（产品与发布线）。

**两边都要的改动只在 master 提交**，然后 `git checkout main-ui && git merge master`；
main-ui 独有的代码（`git cat-file -e master:<路径>` 报错的那些）直接在 main-ui 提交。
两边各提交一份等价的改动，merge 时会把同一段代码合出重复的两份。

功能分支走 `feature/<名字>`，主仓库和要改的 submodule **同名**。

## submodule 改动的硬约束

1. **submodule 的提交必须先推到它自己的远程**，主仓库的指针才有意义——
   没推送的话，别人拉下来构建会直接失败。
2. 合并前跑 `./scripts/check-submodules.sh` 检查指针。
3. **操作 submodule 前先确认当前目录**：同一个相对路径在主仓库和 submodule 里
   指向两个不同的地方，切错目录会改错仓库。
4. 维护者合并时，主仓库的指针要指回 submodule 的**默认分支**，不是 feature 分支。

完整的开发者 / 维护者流程（含每一步的命令和检查清单）见
`docs/engineering/workflow.md`。
