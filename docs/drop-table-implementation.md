# DROP TABLE 实现说明

本次仅实现题目要求的 `DROP TABLE table_name;`，以及完成删除所必需的资源清理和重启恢复衔接。

## 1. 先理解：数据库中的一张表不只是一个文件

假设执行：

```sql
CREATE TABLE students(id INT, score INT);
CREATE INDEX idx_id ON students(id);
INSERT INTO students VALUES (1, 90);
```

MiniOB 会在磁盘和内存中管理这张表的多种资源：

| 资源 | 作用 | 删除时的处理 |
| --- | --- | --- |
| `students.table` | 元数据，记录列的名称、类型、索引定义等 | 删除文件 |
| `students.data` | 保存实际的行数据 | 先关闭，再删除文件 |
| `students-idx_id.index` | 根据 id 查找行的索引数据 | 先释放索引对象、关闭文件，再删除文件 |
| `students.lob`（可能不存在） | 代码中预留的大对象文件 | 如果存在则删除 |
| 数据库中的 `Table` 对象 | 内存中代表这张表的对象 | 释放对象 |
| `opened_tables_` 中的条目 | 让数据库可以通过表名查找 `Table` | 移除条目 |
| 索引对象和文件缓存 | 加速读写的内存资源 | 通过已有析构流程释放 |
| `Catalog` 中的表统计信息 | 例如行数，供优化器使用 | 移除该表对应的统计信息 |

所以 `DELETE FROM students;` 和 `DROP TABLE students;` 的意义不同：前者删除行，表结构仍保留；后者连同表结构、数据和索引一起删除。

## 2. SQL 怎样到达真正的删除代码

```text
DROP TABLE students;
        ↓
已有 SQL 解析器：识别 DROP TABLE，提取表名 students
        ↓
Stmt::create_stmt：创建 DropTableStmt，并检查表是否存在
        ↓
CommandExecutor：选择 DropTableExecutor
        ↓
DropTableExecutor：取得当前数据库，调用 Db::drop_table
        ↓
Db::drop_table：释放内存对象，删除相关文件
        ↓
已有 DDL 后同步流程：Db::sync
        ↓
把执行结果返回给客户端
```

原代码的 SQL 语法规则已经支持 `DROP TABLE`，`StmtType::DROP_TABLE` 也已经存在，因此没有修改词法、语法或新增其他 SQL 语法。

## 3. 每部分修改及原因

### 语句对象：`drop_table_stmt.h/.cpp`、`stmt.cpp`

新增 `DropTableStmt`，保存表名，返回类型 `StmtType::DROP_TABLE`。在 `Stmt::create_stmt` 中增加 `SCF_DROP_TABLE` 分支。

这里检查数据库中是否存在这张表；不存在时返回 `RC::SCHEMA_TABLE_NOT_EXIST`。普通文本协议通常把错误显示为 `FAILURE`。

语句对象保存的是表名字符串，没有长期保存 `Table *`。因为执行 DROP 后 `Table` 会被释放，而语句对象还需要保留到请求处理结束；保存独立的表名更容易避免引用已释放对象。

### 执行器：`drop_table_executor.h/.cpp`、`command_executor.cpp`

新增执行器并接入命令分发。执行器只负责从请求中取得表名和当前数据库，然后调用 `Db::drop_table`。

这样沿用项目现有的结构：SQL 层处理 SQL 请求，存储层处理表和文件。执行器不需要知道索引文件叫什么，也不需要自己管理缓存。

### 真正删除：`db.h/.cpp`

`Db` 已经管理 `opened_tables_`，其中保存“表名 → Table 指针”，因此删除入口放在 `Db` 中。

`Db::drop_table` 按以下顺序操作：

1. 查找表。找不到就返回“表不存在”，不删除文件。
2. 在 Table 仍然有效时，读取它的元数据，保存全部索引文件、数据文件、大对象文件和元数据文件的完整路径。
3. 删除该表的统计信息，并从 `opened_tables_` 移除该表。
4. 执行 `delete table`，复用已有析构流程释放内存资源并关闭文件。
5. 按保存的准确路径逐一删除文件。
6. 删除成功后，记录下次数据库同步需要跳过这张表的旧日志。

为什么先保存路径？因为 `delete table` 后，表元数据也会释放，此后再调用 `table->name()` 或读取索引定义，就会访问已经失效的内存。

为什么按元数据列举文件？因为 `students` 和 `students_backup` 可能同时存在。直接按照表名前缀批量删文件容易误删其他表；列举准确路径能限定删除范围，也能删除该表的所有索引。

为什么先关闭再删除文件？因为打开的文件句柄、缓存、索引对象需要一起清理。只删除磁盘路径，不能自动把数据库自己的这些资源都释放掉。

文件通过 `filesystem::remove(file, ec)` 删除：可选的 `.lob` 文件不存在时不算错误；真实的文件系统错误会记录日志并返回 `RC::FILE_REMOVE`，并继续尝试清理剩余文件。

### 内存释放：复用原有析构链

这部分已有代码足够使用，没有额外重写存储引擎：

```text
delete Table
    ├─ 释放 LobFileHandler（如果有）
    └─ unique_ptr 自动释放 HeapTableEngine
           ├─ 释放 RecordFileHandler
           ├─ 关闭数据文件及其 buffer pool
           └─ 逐一 delete Index
                  └─ 关闭索引的 B+ 树及其 buffer pool
```

buffer pool 可以理解成数据库自己的“文件页面缓存”。原有 `DiskBufferPool::close_file` 会清理缓存页、处理 double write buffer 中对应的页、关闭文件，并从 `BufferPoolManager` 中注销该文件。

这里不能只对索引指针容器调用 `clear()`：容器中的元素是指针，清空容器本身并不会自动 `delete` 指针指向的对象。已有的 `HeapTableEngine` 析构函数已经逐一释放索引，因此直接复用。

### 统计信息：`catalog.h/.cpp`

增加 `remove_table_stats(table_id)`，在原来的互斥锁保护下执行 `table_stats_.erase(table_id)`。

原因是统计信息由全局 `Catalog` 管理，不属于 `Table` 的成员，不能指望 `delete table` 自动删除它。本次只增加删除接口，没有调整统计信息的其他功能。

## 4. 为什么还需要处理日志恢复

数据库重启时不仅会读取 `.table` 文件，还会重放日志。可以把日志理解成一份按顺序编号的操作记录；这个编号叫 LSN。

原代码的重放从指定 LSN 开始，包含这个编号。例如当前最后一条日志为 100：如果删表后仍保存 100 作为恢复起点，下次重启可能再次执行第 100 条日志。如果它指向已删除的表或索引，就会找不到对应的 buffer pool，导致数据库启动失败。

因此增加与 DROP 相关的处理：

- `Db::drop_table` 成功后设置 `dropped_table_since_sync_`。
- `Db::sync` 在其余表及 double write buffer 同步完成、已有日志写入完成后，保存 `current_lsn + 1` 作为 DROP 后的恢复起点。
- 检查点取新旧起点的较大值，避免退出时再次同步把起点退回旧日志。
- double write buffer 同步失败时立即返回，不能把失败当成已完成并推进检查点。
- `DiskLogHandler::replay` 从至少 `start_lsn - 1` 初始化已知最大 LSN。即使恢复起点之后没有日志，重启后新日志也会继续正确编号，不会重新从 1 开始。

最后这一点很重要：删除后能够重启，还必须能够在重启后重新建表、写入数据并再次重启。

日志是整个数据库共享的，其中可能包含其他表的记录，因此这里没有直接删除整个日志目录。

## 5. 已完成的验证

开发时通过临时定向测试检查了以下场景；按要求，测试源文件不包含在本次提交中：

- SQL 能解析为 DROP TABLE，并生成正确的语句对象。
- 删除空表、重复删除和删除不存在的表。
- 删除含数据、两个索引、可选大对象文件和统计信息的表。
- 名称相近的另一张表仍然保留。
- 对应的文件不再登记在 buffer pool 管理器中，缓存页数量恢复到删除前的基线。
- 可以重建同名表和同名索引，且新表中没有旧数据。
- 删除从磁盘加载的表后，经过重复同步仍可重启。
- 重启后重建并插入数据，再次重启仍能读取新记录。

在 Linux/MiniOB 开发容器中重新配置和构建数据库：

```bash
cmake -S . -B build
cmake --build build --target observer -j 6
```

验证时构建开启了 AddressSanitizer，临时测试运行时关闭了全局泄漏扫描；缓存和文件资源的释放另外通过测试中的断言检查。

验证结果：数据库服务和测试均编译成功，3 个定向单元测试全部通过。另通过实际 SQL 连接，在默认事务模型的内存日志、磁盘日志两种配置下完成了 50 条 SQL 检查，覆盖删后查询/插入/删除/描述表失败、SHOW TABLES、磁盘文件删除、重建同名表和索引、多次重启后的数据读写。

额外尝试 MVCC 配置时，删除目标表之前的查询未返回刚插入的数据，因此未把这组检查列为通过；本次没有扩展修复事务查询功能。

本实现沿用项目已有的约束：DDL 和同步期间不并发访问正在删除的表，没有增加表锁、DDL 回滚或删除中途断电的原子恢复机制。
