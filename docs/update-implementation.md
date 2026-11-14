# 单字段 UPDATE 实现与入门讲解

本次只补齐题目要求的单字段常量赋值：

```sql
UPDATE t SET id = 1 WHERE age = 1;
UPDATE t SET age = 18;
```

支持按已有 WHERE 条件更新一行或多行、不写 WHERE 时更新全部行，以及表上存在普通索引时的更新。没有扩展多字段赋值、`SET age = age + 1`、子查询赋值、NULL、唯一索引、其他存储引擎或其他题目功能。

## 1. 先把 SQL 和内核概念对应起来

假设有这样一张表：

| id | name | age |
| --- | --- | --- |
| 1 | Alice | 18 |
| 2 | Bob | 20 |
| 3 | Carol | 20 |

执行：

```sql
UPDATE t SET age = 21 WHERE id = 2;
```

数据库需要依次回答四个问题：

1. 表 `t` 和列 `age`、`id` 是否存在？数字 `21` 能否存进 `age`？
2. 哪些行满足 `id = 2`？
3. 怎样保留这一行的 `id` 和 `name`，只替换 `age`？
4. 如果有索引，怎样保证更新后仍能正确查找？

下面几个术语只是这些工作的代码名称：

| 代码概念 | 可以怎样理解 |
| --- | --- |
| `Table` | 内存中代表一张表的对象 |
| `TableMeta` | 表的说明书，记录有哪些列、每列类型和索引等 |
| `FieldMeta` | 某一列的说明书，包括类型、长度、在行中的位置 |
| `Value` | 带有类型的一个值，例如整数 21、字符串 Bob |
| `Record` | 一行的原始字节数据 |
| `RID` | 一行的物理地址，包含数据页号和页内槽位号 |
| `FilterStmt` / Predicate | WHERE 条件；Predicate 常翻译成“谓词”，这里理解为判断条件即可 |
| 扫描算子 | 逐行取出数据的执行组件，可以遍历整张表，也可以通过索引找行 |
| `Trx` | 事务接口；执行增删改时通过它进入具体事务实现 |
| `RC` | 函数的执行结果，例如成功、表不存在、扫描结束 |

## 2. 原代码为什么不能执行 UPDATE

原有 `yacc_sql.y` 已经能够识别：

```text
UPDATE 表名 SET 列名 = 常量 [WHERE 条件]
```

解析器会把表名、列名、新值和条件存进 `UpdateSqlNode`。但是“认识 SQL 的语法”不等于“能执行 SQL”：

- `UpdateStmt::create` 还是 TODO，并直接返回错误。
- `Stmt::create_stmt` 没有分发 `SCF_UPDATE`。
- 没有 UPDATE 对应的逻辑计划、物理计划和执行算子。
- 默认事务实现的 `update_record` 也是未实现状态。

因此没有改动语法文件，而是补齐解析之后的执行链路。

## 3. 一条 UPDATE 现在怎样执行

```text
UPDATE t SET id = 1 WHERE age = 1;
                 ↓
已有语法解析器：得到表名、列名、赋值常量和 WHERE
                 ↓
Stmt::create_stmt → UpdateStmt::create
检查表、字段、赋值类型、WHERE 中引用的字段
                 ↓
LogicalPlanGenerator：生成“取行 → 过滤 → 更新”的计划
                 ↓
PhysicalPlanGenerator：选择全表扫描或已有索引扫描
                 ↓
UpdatePhysicalOperator
先复制所有匹配的旧行，再关闭扫描
                 ↓
Table::make_update_record
复制旧行，只替换目标字段，得到新行
                 ↓
Trx::update_record
通过已有 INSERT / DELETE 路径替换行，并维护索引
                 ↓
沿用已有结果返回流程，客户端收到 SUCCESS 或 FAILURE
```

所谓“逻辑计划”是描述要做什么；“物理计划”是决定用什么具体执行组件来做。它们是 MiniOB 已有的架构，DELETE 也按类似过程执行。

例如在 `WHERE id = 2` 且 `id` 有索引时，已有计划生成器可以选择索引扫描；没有可用索引时就遍历表。UPDATE 复用这些选择逻辑。

## 4. 各部分改了什么，为什么这样改

### 4.1 语句检查：update_stmt.h / update_stmt.cpp / stmt.cpp

`UpdateStmt` 现在保存四样东西：目标表、目标列、赋值常量、过滤条件。它返回 `StmtType::UPDATE`，并负责释放自己拥有的过滤条件。

创建语句时：

1. 通过表名找 `Table`，找不到就失败。
2. 从表的元数据中找目标列；禁止把内部隐藏字段当成用户列更新。
3. 检查新值的类型。类型不同时使用已有 `Value::cast_to`，与 INSERT 复用同一套转换规则；例如整数可以转换为 FLOAT。不能转换时返回失败，不新增宽松转换规则。
4. 使用已有 `FilterStmt::create` 检查 WHERE，包括条件中不存在的列。
5. 成功后创建 `UpdateStmt`。

为什么提前检查？例如：

```sql
UPDATE t SET missing_column = 1 WHERE id = 999;
```

即使没有 `id = 999` 的行，引用不存在的列仍是错误，不能因为没查到行就偷偷当成成功。赋值类型也是同样道理。

### 4.2 执行计划：两个 plan_generator 和 UPDATE 逻辑算子

新增 `UpdateLogicalOperator`，携带表、目标列、新值。逻辑计划生成器仿照 DELETE，创建可写的取表算子，并接上已有条件过滤。

有 WHERE 时，可以理解为：

```text
UPDATE
  └─ 条件过滤
       └─ 取表中的行
```

没有 WHERE 时直接取出全部行。已有优化器可能把过滤条件下推到扫描算子中，这不改变执行语义。

物理计划生成器把上述计划转换为 `UpdatePhysicalOperator` 及其扫描/过滤子算子。

同时新增 UPDATE 算子类型和名称，并在 `can_generate_vectorized_operator` 中把 UPDATE 与 INSERT、DELETE 一样标为使用逐行执行路径。这样即使会话选择了 CHUNK 模式，UPDATE 仍能用本次实现的逐行算子执行。

### 4.3 实际执行：update_physical_operator.h / update_physical_operator.cpp

这个组件负责组织工作，不直接修改磁盘文件。核心步骤是：

```text
打开扫描
while 还有满足条件的行:
    将旧行的字节和 RID 复制到独立内存
关闭扫描

for 每条已收集的旧行:
    生成只替换目标字段的新行
    调用事务的 update_record
```

**为什么先收集，再修改？**

考虑：

```sql
UPDATE t SET id = 10 WHERE id = 9;
```

如果 `id` 有索引，数据库可能正在沿着 `id = 9` 的索引条目向后找行。此时一边遍历、一边删除索引条目并插入 `id = 10`，就会干扰正在使用的扫描位置，可能漏行、重复处理，甚至与扫描持有的锁冲突。

先确定本次要改的行，再关闭扫描并修改，意味着本条 SQL 新生成的行不会再次进入本次扫描。

**为什么要复制字节，不能只记一个指针？**

扫描器返回的行可能借用数据页里的内存。扫描切换页面或关闭后，这块内存不再保证有效。这里显式 `copy_data`，使每一行都拥有自己的内存，并单独保存 RID；这样更新跨多个数据页的数据也能正常工作。

扫描错误和正常扫描结束也必须区分。`RECORD_EOF` 表示“没有下一行”，是正常结束；其他错误则停止执行，不开始修改已收集的行。执行状态会由 `close()` 返回给已有结果处理流程，便于该流程处理失败和事务回滚。

### 4.4 构造新行：Table::make_update_record

这里并不重新解析整条 SQL，而是：

1. 复制旧行全部字节，保留其他列。
2. 根据 `FieldMeta::offset()` 找到目标列在行中的位置。
3. 将目标列占用的空间清零。
4. 调用 INSERT 已使用的 `set_value_to_record` 写入新值。

例如，简化后的行布局可能是：

```text
id: 4 字节 | name: 8 字节 | age: 4 字节
offset=0     offset=4       offset=12
```

修改 age，就是修改从第 12 字节开始的 4 字节；其他部分保持原样。真实布局还可能包含内部事务字段，所以使用元数据里的偏移量，不能把这些位置写死。

清零尤其用于字符串：把 `Alice` 改成 `B`，如果没有正确覆盖旧内容，可能留下旧字符串尾部。新的写入代码先清空这个字段，再沿用 INSERT 的写入和长度处理规则。

### 4.5 行替换和索引：vacuous_trx / mvcc_trx

本题没有要求必须在原来的 RID 上原地修改。这里采用行替换方式，复用已有且已经处理索引的增删路径，避免另写一套容易遗漏的索引更新代码。

默认的 `VacuousTrx` 不提供完整事务功能，它的更新顺序是：

```text
插入新行（并插入新行的索引条目）
删除旧行（并删除旧行的索引条目）
```

先插入的原因是：如果新行插入失败，旧行还保留着。删除旧行失败时，会尝试删除刚插入的新行。此处只处理替换当前行所需的错误清理，不扩展 VacuousTrx 为支持整条多行语句回滚的事务系统。

**索引为什么也正确？**

可以把普通索引理解成“列值 → 行地址”的目录。原来可能是：

```text
id=3 → 旧 RID
```

更新 `id=4` 后，在默认事务模式下变为：

```text
id=4 → 新 RID
```

新行插入走 `HeapTableEngine::insert_record`，它会为表上的索引添加条目；旧行删除走 `HeapTableEngine::delete_record`，它会删除旧条目。即使修改的是没有索引的列，新行 RID 也可能改变，因此仍需同步表上所有索引的地址映射；已有增删代码已经完成这件事。

同样的列值可以对应多个不同 RID，本题使用的是普通索引。因此把多行更新成同一个值，或者把某列更新为原值，都是允许的。

`MvccTrx` 的 `update_record` 则复用已有“删除旧版本 → 插入新版本”的事务操作。这里的删除是给旧版本记录可见性信息，而不是立即物理删除；旧索引条目可继续存在，由已有事务可见性检查决定该版本是否应当返回。本次没有新建 UPDATE 日志类型，也没有重写原有 MVCC 的并发、回滚、恢复机制。

## 5. 修改文件导航

| 文件 | 职责 |
| --- | --- |
| `src/observer/sql/stmt/stmt.cpp` | 分发 UPDATE 语句 |
| `src/observer/sql/stmt/update_stmt.h/.cpp` | 检查表、列、新值和条件，保存更新信息 |
| `src/observer/sql/optimizer/logical_plan_generator.h/.cpp` | 生成取行、过滤和更新计划 |
| `src/observer/sql/optimizer/physical_plan_generator.h/.cpp` | 创建实际执行的 UPDATE 算子 |
| `src/observer/sql/operator/update_logical_operator.h` | 保存逻辑更新操作 |
| `src/observer/sql/operator/update_physical_operator.h/.cpp` | 收集匹配行，关闭扫描，逐行更新 |
| `src/observer/sql/operator/logical_operator.h/.cpp` | 注册逻辑类型，选择逐行执行 |
| `src/observer/sql/operator/physical_operator.h/.cpp` | 注册物理类型及显示名称 |
| `src/observer/storage/table/table.h/.cpp` | 按列偏移量构造新行 |
| `src/observer/storage/trx/vacuous_trx.h/.cpp` | 默认事务模式下通过增删替换行 |
| `src/observer/storage/trx/mvcc_trx.h/.cpp` | 复用已有版本增删操作 |

## 6. 自己动手验证

编译后，在项目根目录的 Linux 环境运行：

```bash
cmake -S . -B build
cmake --build build --target observer -j 4
```

开发时使用临时脚本启动真正的 observer，通过 SQL 协议请求数据库，再检查 SELECT 返回的数据。测试数据放在独立临时目录，不使用原来的数据库文件。验证完成后已清理临时测试脚本和日志，最终仅保留功能代码和本文档。

本次验证结果：observer 编译成功，开启 AddressSanitizer，运行时关闭全局泄漏扫描；以下四组共通过 **942 条 SQL 检查**（包含测试数据插入和 SELECT 结果断言）：

| 配置 | SQL 检查数 |
| --- | --- |
| 默认 VacuousTrx + 内存日志 | 230 |
| 默认 VacuousTrx + 磁盘日志 | 230 |
| MvccTrx + 内存日志 | 241 |
| MvccTrx + 磁盘日志 | 241 |

覆盖有条件/无条件更新、多行更新、AND 条件、空表、零匹配、索引查旧值和新值、重复索引键、更新为原值、字符串变短、FLOAT 赋值转换、无效表/列/类型、CHUNK 会话中的 UPDATE，以及 180 行跨数据页更新。MVCC 组另检查了无索引表上的显式提交和回滚。

验证边界：上述通过结果针对运行中的 SQL。额外重启检查遇到了现有恢复流程的读错误；大批量操作后执行 DDL 触发同步也出现超时。后者用只包含原有 INSERT/DELETE、完全不执行 UPDATE 的对照程序同样能复现。因此没有把重启恢复列为本次通过项，也没有扩展修改恢复或同步模块。默认 VacuousTrx 本身不具备完整事务回滚保证；本次不增加这种保证。

也可以在客户端逐条运行：

```sql
CREATE TABLE t(id INT, age INT, name CHAR(8));
CREATE INDEX idx_id ON t(id);
INSERT INTO t VALUES(1, 18, 'Alice');
INSERT INTO t VALUES(2, 20, 'Bob');
INSERT INTO t VALUES(3, 20, 'Carol');

UPDATE t SET name = 'B' WHERE id = 2;
SELECT * FROM t WHERE id = 2;
-- 应看到 2 | 20 | B

UPDATE t SET id = 9 WHERE age = 20;
SELECT * FROM t WHERE id = 2;
-- 应没有数据
SELECT * FROM t WHERE id = 9;
-- 应看到 Bob 和 Carol 原来对应的两行，name 分别是 B、Carol

UPDATE t SET age = 21;
SELECT * FROM t;
-- 三行的 age 都应为 21
```

注意：没有 WHERE 就会更新全部行，这是 SQL 本身的含义；没有匹配行则返回成功，数据保持原样。
