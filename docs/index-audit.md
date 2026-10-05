# 只读 index 审计（read-only index audit）

本审计基于 libgit2 实现，参考实现与验收测试位于
`tests/libgit2/index/audit.c`（clar 套件 `index::audit`）。
审计对 index 与工作区**完全只读**：不刷新（refresh）、不写回、不删除任何
index 条目，也不修改工作区文件。

## 状态分类

每个 index 条目被归入且仅归入以下一种状态：

| 状态 | 判定依据 |
| --- | --- |
| 普通条目（normal） | stage 0，无 intent-to-add 标志，且工作区文件存在 |
| intent-to-add | stage 0，且 `flags_extended` 含 `GIT_INDEX_ENTRY_INTENT_TO_ADD` |
| 冲突 stage（conflict） | `flags` 中 stage 位（`GIT_INDEX_ENTRY_STAGEMASK`）为 1/2/3 |
| 缺失工作区内容（missing worktree） | stage 0，无 intent-to-add 标志，工作区文件不存在 |

判定优先级：冲突 stage > intent-to-add > 缺失工作区 > 普通条目。
冲突身份**只**来自 flags 中的 stage 位，与路径、大小写无关。

每条审计记录都完整保留：

- 完整 OID（`git_oid`，未截断）；
- 原始 `flags`（含 stage 位）与 `flags_extended`（含 intent-to-add /
  skip-worktree 位）；
- 解码后的 stage 与分类状态。

## 只读保证

- 审计只调用 `git_index_entrycount` / `git_index_get_byindex` 读取条目，
  并对工作区路径做只读 `stat`（`git_fs_path_exists`）判断文件是否存在。
- **绝不**为了“看清”缺失内容而调用 `git_index_refresh` 或写回 index；
  缺失内容就是缺失，如实报告即可。
- 验收：`test_index_audit__does_not_modify_index` 断言审计前后
  `.git/index` 文件字节完全相同，且内存中的 index 条目不变。

## 澄清：缺文件 ≠ 删除已暂存内容

工作区文件被删除**不等于**已暂存（staged）内容被删除。条目仍以原 OID
留在 index 中；审计将其报告为“缺失工作区内容”，同时保留其暂存 OID 与
flags，且不会从 index 中移除该条目（refresh 才会那么做）。在没有冲突时，
该仓库仍是可提交的——提交会包含已暂存的 blob。
定向测试：`test_index_audit__missing_file_is_not_a_staged_removal`。

## 大小写碰撞：仅作独立诊断

在大小写不敏感的平台（如 Windows/macOS 默认文件系统）上，仅大小写不同
的路径（如 `CaseFile.txt` 与 `casefile.txt`）会在工作区发生碰撞。审计将
此类路径对报告为**独立诊断**（`case_collisions`），它：

- 不参与、也不覆盖任何条目的分类；
- 不改变任何条目的真实 stage 身份（stage 只来自 flags 的 stage 位）。

定向测试：`test_index_audit__case_collision_is_diagnostic_only`。

## 可提交状态摘要

审计报告汇总各状态条目数（normal / intent-to-add / 冲突条目数 /
冲突路径数 / 缺失工作区数），并给出可提交状态：

- `committable = true` 当且仅当 index 中没有任何冲突 stage 条目；
- intent-to-add 与缺失工作区条目不阻塞提交（语义见上文澄清）。

## 运行方式（定向、串行）

构建（在仓库根目录）：

```sh
cmake -S . -B build -G Ninja -DBUILD_CLI=OFF -DBUILD_TESTS=ON
cmake --build build --target libgit2_tests
```

定向运行本审计套件（clar 本身串行执行）：

```sh
./build/libgit2_tests -sindex::audit
```

通过 ctest 驱动时使用 `-j1` 强制串行（`index_audit` 是本审计套件的定向 ctest 条目）：

```sh
ctest --test-dir build -R index_audit -j1
```
