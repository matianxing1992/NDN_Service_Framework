# Development Branch and Spec Lifecycle Policy

## English

`main` is the stable integration branch. `Experimental` is the active
integration branch for validated experimental NDNSF work. A Spec branch is a
temporary implementation workspace unless Tianxing explicitly asks to retain
it.

The required lifecycle is:

1. Create a temporary branch for a Spec or bounded task when isolation is
   useful.
2. Keep implementation, tests, documentation, and evidence in that branch
   until the task is complete.
3. Before closing the task, merge the completed branch into `Experimental`.
4. After `Experimental` validation, merge the validated integration state into
   `main`.
5. Do not infer that a branch was merged from its name; verify ancestry and the
   resulting tree. Preserve `main`, `Experimental`, and branches explicitly
   requested by Tianxing. Other temporary branches may be removed only after
   their useful commits and evidence are preserved.

Never use `git add -A` in the shared worktree to close a Spec. Generated files,
unrelated dirty edits, and untracked evidence must be reviewed and assigned to
the correct branch before committing. A merge is incomplete if the Spec's
implementation is present but its required tests, contracts, or evidence have
been left only in an untracked worktree.

## 中文

`main` 是稳定集成分支，`Experimental` 是经过验证的 NDNSF 实验工作的主集成
分支。Spec 分支默认只是临时工作区，只有 Tianxing 明确要求保留时才作为长期
分支保留。

规定的生命周期是：

1. 需要隔离时，为一个 Spec 或边界清晰的任务创建临时分支。
2. 在该分支中完成实现、测试、文档和证据。
3. 任务完成前，将完整分支合并到 `Experimental`。
4. `Experimental` 验证通过后，再将验证后的集成状态合并到 `main`。
5. 不能根据分支名称推断是否已经合并，必须检查 ancestry 和合并后的文件树。
   只保留 `main`、`Experimental` 以及 Tianxing 明确要求保留的分支；其他临时
   分支只有在提交和证据已保存后才可以清理。

共享工作树中禁止用 `git add -A` 结束 Spec。生成文件、无关脏改动和未跟踪证据
必须先审查并归属到正确分支。若 Spec 的实现已经合并，但所需测试、契约或证据
仍只存在于未跟踪工作树，则该 Spec 不能视为完成合并。
