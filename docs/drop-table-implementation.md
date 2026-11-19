# DROP TABLE 实现详解

本次实现题目要求的 `DROP TABLE table_name;`，重点是把表从当前数据库中彻底移除：SQL 请求能到达存储层，表及索引的内存资源先正确关闭，随后删除准确的磁盘文件，并保证重启时不会重放已删除表的旧日志。

本文面向只有 SQL 基础的读者。先看最重要的执行流程和设计原因，再列出每个改动文件。不是本题重点的接口接线和已有机制只简要说明。

## 1. 最终效果与执行流程

```sql
DROP TABLE students;
```

`DROP TABLE` 删除整张表，包括表结构、数据和索引。它不同于 `DELETE FROM students;`：`DELETE` 删除行，但表还在；`DROP TABLE` 删除表本身。删除后再查询该表会报表不存在，表名可以重新使用。

SQL 在 MiniOB 里的主要路径是：

```text
SQL 文本
  -> 解析器产出 SCF_DROP_TABLE 和表名
  -> Stmt::create_stmt 创建 DropTableStmt
  -> CommandExecutor 分发给 DropTableExecutor
  -> Db::drop_table 关闭资源并删除文件
  -> DDL 通用流程调用 Db::sync
```

几个术语：`Stmt` 是解析并做过基本检查后的语句对象；`Executor` 是负责执行该类语句的组件；`Db` 是当前数据库的对象，管理已打开的表；`Table` 是内存中代表一张表的对象；buffer pool 是数据库用来缓存磁盘页的内存区域；LSN 是日志记录的顺序编号；checkpoint（检查点）决定重启时从哪条日志开始恢复。

## 2. 关键修改一：Db 负责完整删除

主要文件：`src/observer/storage/db/db.h`、`src/observer/storage/db/db.cpp`。

`Db` 原本就通过 `opened_tables_` 保存“表名到 `Table*`”的映射，所以删除入口放在 `Db`，而不是让 SQL 执行器直接碰文件。新增的 `Db::drop_table` 是核心实现，按这个顺序工作：

1. 在 `opened_tables_` 中查找目标表。找不到就返回 `SCHEMA_TABLE_NOT_EXIST`，不做文件删除。
2. 趁 `Table` 和它的元数据仍然有效，收集 `.table` 元数据文件、`.data` 数据文件、`.lob` 可选文件，以及元数据中列出的每个索引文件的完整路径。
3. 清除该表的 Catalog 统计信息，并从 `opened_tables_` 移除映射，防止数据库继续按表名找到它。
4. `delete table`，走已有析构函数，释放记录管理器、索引对象和文件缓存。
5. 删除之前保存的文件路径。真实文件系统错误记日志并返回 `RC::FILE_REMOVE`；不存在的可选 `.lob` 文件不算错误。
6. 所有文件删除成功后，标记下次同步需要推进恢复检查点。

先保存文件路径是为了避免悬空指针。`delete table` 后，`table` 和 `TableMeta` 都已经释放，再从 `table->name()` 或索引元数据取文件名就是访问已失效内存。按索引元数据逐一构造准确路径，也避免按表名前缀删除时误伤 `students_backup` 这类相似名称的表。

顺序上先关闭资源、再删文件。Windows 等系统通常不允许删除仍被打开的文件；即使文件系统允许 unlink，数据库进程里遗留的句柄和缓存也会让后续重建同名表变得不可靠。

`db.h` 只增加公开的 `drop_table` 声明和一个状态位 `dropped_table_since_sync_`。状态位用于下一节的检查点处理。

## 3. 关键修改二：复用析构链关闭文件和缓存

删除文件之前要释放的不是单个文件句柄，而是围绕表构建的一串对象。这里没有重写存储引擎，而是复用已有析构流程：

```text
delete Table
  -> Table::~Table
     -> 释放 HeapTableEngine
        -> 删除 RecordFileHandler
        -> 关闭数据文件和它的 buffer pool
        -> 逐个 delete 索引
           -> B+ 树索引关闭索引文件
     -> 释放可选的 LobFileHandler
```

buffer pool 里的页可能还指向待删除文件。`DiskBufferPool::close_file` 会清掉该文件相关缓存页和 double-write 状态，关闭文件句柄，并把文件从 `BufferPoolManager` 注销。若只调用文件删除函数，内存中的页和句柄并不会自动跟着消失。

这里使用 `delete` 而不只是清空索引指针数组：数组存的是裸指针，清空容器本身不会释放指针所指对象。`HeapTableEngine` 的析构函数已经逐个删除索引，因而直接利用已有逻辑。

## 4. 关键修改三：检查点必须越过旧日志

只删文件和内存还不够。MiniOB 重启时会读取数据库检查点，然后从该 LSN（包含该编号）开始重放日志。旧日志可能包含对这张表或它的索引文件的修改；若重启后再重放这些日志，文件已经删掉，恢复可能失败。

核心修改在 `Db::sync`：成功删除表后设置 `dropped_table_since_sync_`。下次同步先刷新仍然存在的表和 double-write buffer，再等待当前日志写入完成，最后将检查点推进到至少 `current_lsn + 1`。因为重放包含起点本身，`+1` 才能跳过截至 `current_lsn` 的旧记录。

这里还有两个容易忽略的细节：

- 检查点取“原值”和新候选值中的较大者。删除命令成功后已经自动同步一次，而进程退出时可能再同步；如果第二次同步把检查点降回当前 LSN，重启就会重新触碰旧日志。
- double-write buffer 刷新失败时直接返回，不推进检查点。否则检查点可能越过尚未安全落盘的数据。

同步写入数据库元数据文件成功后才清除删除状态。数据库日志是多个表共享的，不能为了删除一张表就删掉整个日志目录，否则会丢失其他表需要恢复的修改。

`src/observer/storage/clog/disk_log_handler.cpp` 还有一处配套调整：如果检查点之后没有新日志，磁盘日志回放仍要把“已知最大 LSN”初始化为 `start_lsn - 1`。否则重启后的新日志编号可能从 1 重新开始，与旧编号冲突，后续恢复又会出错。

## 5. SQL 层接线与辅助清理

这些改动对理解资源释放和恢复不是核心，但它们把新命令接入项目已有的 SQL 结构。

### 语句对象

`src/observer/sql/stmt/drop_table_stmt.h` 和 `.cpp` 新增 `DropTableStmt`：保存表名、报告 `StmtType::DROP_TABLE`，并在创建语句时检查表是否存在。它保存表名字符串而不是 `Table*`，因为执行器稍后会删除那个 `Table` 对象。`src/observer/sql/stmt/stmt.cpp` 增加 `SCF_DROP_TABLE` 到 `DropTableStmt::create` 的分支。

解析器的 `DROP TABLE` 语法和 `StmtType::DROP_TABLE` 已经存在，本次没有改词法/语法。`stmt_type_ddl` 也已有 DROP_TABLE，因此成功的 DDL 会进入现有的 `Db::sync` 逻辑。

### 执行器和命令分发

`src/observer/sql/executor/drop_table_executor.h/.cpp` 新增轻量执行器：从 SQL 事件取得语句和当前数据库，再把表名交给 `Db::drop_table`。`command_executor.cpp` 增加一个 `DROP_TABLE` 分支选择此执行器。执行器不负责拼路径、释放索引或操作 buffer pool，这些属于存储层 `Db` 的职责。

### Catalog 统计

`src/observer/catalog/catalog.h/.cpp` 增加 `remove_table_stats(table_id)`，在现有互斥锁保护下移除表统计信息。Catalog 是单独的全局统计信息管理对象，不属于 `Table`，所以销毁 `Table` 不会自动清掉这项数据。

文件路径的拼装复用了已有的 `table_meta_file`、`table_data_file`、`table_index_file` 和 `table_lob_file`（定义在 `src/observer/storage/common/meta_util.cpp`），本次没有改这些工具函数。

## 6. 修改文件总览

| 文件 | 修改内容 | 重要性 |
| --- | --- | --- |
| `src/observer/storage/db/db.cpp` | 删除顺序、资源关闭、文件移除、检查点状态及同步处理 | 核心 |
| `src/observer/storage/db/db.h` | 声明删除接口和同步状态 | 核心配套 |
| `src/observer/storage/clog/disk_log_handler.cpp` | 无新日志时保留正确的最大 LSN，避免编号重用 | 恢复配套 |
| `src/observer/sql/stmt/drop_table_stmt.h/.cpp` | 保存目标表名并检查表存在 | SQL 接线 |
| `src/observer/sql/stmt/stmt.cpp` | 把解析结果转换成 `DropTableStmt` | SQL 接线 |
| `src/observer/sql/executor/drop_table_executor.h/.cpp` | 将执行请求交给当前数据库 | SQL 接线 |
| `src/observer/sql/executor/command_executor.cpp` | 把 DROP TABLE 分发到新执行器 | SQL 接线 |
| `src/observer/catalog/catalog.h/.cpp` | 删除目标表的内存统计信息 | 辅助清理 |
| `docs/drop-table-implementation.md` | 说明实现方案、取舍和验证情况 | 文档 |

## 7. 验证和实现边界

开发时在 Linux/MiniOB 容器中成功构建 `observer`。另用实际 SQL 连接，在默认事务模型下分别测试内存日志和磁盘日志，共检查 50 条 SQL 场景，包括删除与重复删除、删后访问失败、其他表保留、重建同名表和索引，以及多次重启后新数据仍可读。临时定向测试也曾通过；测试源文件按要求未保留，因此这里不提供不可复现的测试文件命令。

MVCC 配置下的额外尝试在 DROP 之前就读不到刚插入的数据，所以不作为本题实现的通过结果；本次没有扩展修复事务查询功能。当前实现也沿用 MiniOB 原有的 DDL 使用约束，没有增加并发删表锁、跨多个文件的原子回滚或删除中途断电后的自动撤销机制。

阅读源码时，建议依次看 `DropTableStmt::create`、`DropTableExecutor::execute`、`Db::drop_table`、`Table::~Table`、`HeapTableEngine::~HeapTableEngine` 和 `Db::sync`。前两处回答“SQL 怎么到存储层”，中间几处回答“资源怎么释放”，最后一处回答“重启为什么不会再碰已删表的旧日志”。
