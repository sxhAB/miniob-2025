# 单字段 UPDATE 实现详解

> 本文面向只有 SQL 基础、没有数据库内核开发经验的读者。先用 SQL 说明最终效果，再沿执行流程解释每个修改文件、关键代码和设计原因。
>
> 本文讲解的是 `b010370` 提交实现的 UPDATE：共修改或新增 20 个代码文件，另有本文档。仓库后来还有 JOIN、VECTOR 等提交；下面会区分 UPDATE 新增的内容与直接复用的已有功能，不把其他题目的改动算进来。

本次只补齐题目要求的单字段常量赋值：

```sql
UPDATE t SET id = 1 WHERE age = 1;
UPDATE t SET age = 18;
```

支持按已有 WHERE 条件更新一行或多行、不写 WHERE 时更新全部行，以及表上存在普通索引时的更新。没有扩展多字段赋值、`SET age = age + 1`、子查询赋值、NULL、唯一索引、其他存储引擎或其他题目功能。

## 0. 先看最终效果

假设先执行：

```sql
CREATE TABLE students(id INT, age INT, name CHAR(8));
CREATE INDEX idx_id ON students(id);
INSERT INTO students VALUES (1, 18, 'Alice');
INSERT INTO students VALUES (2, 20, 'Bob');
INSERT INTO students VALUES (3, 20, 'Carol');
```

**按条件修改一行：**

```sql
UPDATE students SET name = 'B' WHERE id = 2;
SELECT * FROM students WHERE id = 2;
```

结果应为：

```text
id | age | name
2 | 20 | B
```

**修改多行，并且修改的是索引列：**

```sql
UPDATE students SET id = 9 WHERE age = 20;
SELECT * FROM students WHERE id = 9;
```

结果应包含以下两行，返回顺序不作保证：

```text
id | age | name
9 | 20 | B
9 | 20 | Carol
```

`idx_id` 是普通索引，允许不同的行拥有同一个 id。此时查询 `id = 2` 或 `id = 3` 应当没有数据。

**省略 WHERE，修改全部行：**

```sql
UPDATE students SET age = 21;
```

三行的 age 都变成 21。每条成功的 UPDATE 在普通文本协议中返回 `SUCCESS`；它本身不返回 SELECT 那样的结果表，也没有在本题中新增受影响行数输出。

**错误输入与零匹配：**

```sql
UPDATE students SET missing = 1;             -- FAILURE：目标列不存在
UPDATE students SET age = 'bad';             -- FAILURE：不能完成赋值转换
UPDATE students SET age = 21 WHERE id = 999; -- SUCCESS：没有匹配行，无须修改
```

后面的讲解就围绕这几类行为展开。

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

## 5. 关键代码与配套改动

第 4 节已经说明核心设计，下面补充最值得对照源码阅读的代码。仅用于注册类型、声明接口的修改放在本节末尾简要列出。

### 5.1 提前转换赋值类型，而不是扫描到行后才检查

文件：`src/observer/sql/stmt/update_stmt.cpp`

```cpp
Value value = update.value;
if (value.attr_type() != field->type()) {
  Value cast_value;
  RC rc = Value::cast_to(value, field->type(), cast_value);
  if (rc != RC::SUCCESS) {
    return rc;
  }
  value = std::move(cast_value);
}
```

这里先把常量转换成目标字段的类型，再保存进 UpdateStmt。例如整数赋给 FLOAT 可以复用已有转换；不支持的赋值则提前失败，即使 WHERE 没匹配到行也不会绕过检查。

这同样能衔接 DATE：目标是 DATE、输入是日期字符串时，通过已有类型系统进行日期校验，无须另写 UPDATE 专用解析器。

### 5.2 必须复制行内容，并单独保存旧 RID

文件：`src/observer/sql/operator/update_physical_operator.cpp`

```cpp
const Record &record = static_cast<RowTuple *>(tuple)->record();
Record copy;
rc = copy.copy_data(record.data(), record.len());
if (rc != RC::SUCCESS) {
  break;
}
copy.set_rid(record.rid());
records.emplace_back(std::move(copy));
```

`copy_data` 保存独立的行字节，避免扫描换页或关闭后旧内存失效；`set_rid` 保存旧行的物理地址，供删除旧行使用。内容和地址是两件事，都要保存。

收集完毕后先关闭扫描，并区分正常读完与执行错误：

```cpp
RC close_rc = child.close();
if (rc != RC::RECORD_EOF) {
  return rc;
}
if (close_rc != RC::SUCCESS) {
  return close_rc;
}
```

只有扫描正常结束，才开始修改。没有匹配行时，列表为空，修改循环执行零次并返回成功。

这套做法需要暂存全部匹配旧行，额外内存随匹配行数和行长度增长。本题没有进一步实现大型更新的落盘缓存机制。

### 5.3 复制旧行，只清空并写入目标字段

文件：`src/observer/storage/table/table.cpp`

```cpp
RC Table::make_update_record(const Record &old_record, const FieldMeta &field,
    const Value &value, Record &new_record)
{
  if (field.type() != value.attr_type()) {
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }
  RC rc = new_record.copy_data(old_record.data(), table_meta_.record_size());
  if (rc != RC::SUCCESS) {
    return rc;
  }
  memset(new_record.data() + field.offset(), 0, field.len());
  return set_value_to_record(new_record.data(), value, &field);
}
```

先复制整行，其他列自然保留；再按元数据中的偏移量覆盖一列。清零用于避免字符串变短后残留旧内容，最后复用已有写值方法。

把这件事放在 Table 中，是因为 Table 掌握记录布局和字段写入规则。物理算子负责组织执行，不必重复处理每种字段的字节表示。

此函数只生成内存中的新行，不直接写磁盘，也不把旧 RID 设置给新行；插入时由存储层分配新地址。

### 5.4 保存真实执行状态，正确衔接事务收尾

文件：`src/observer/sql/operator/update_physical_operator.h`、`src/observer/sql/operator/update_physical_operator.cpp`

```cpp
RC UpdatePhysicalOperator::open(Trx *trx)
{
  result_ = execute(trx);
  return result_;
}
```

UPDATE 在 `open()` 中执行整次修改。它没有 SELECT 那样的结果行，所以 `next()` 直接返回结束，`close()` 返回保存的 `result_`。

这个返回值很关键：已有 `SqlResult::close()` 在自动事务模式下，会根据算子的关闭状态决定提交还是回滚。如果更新失败而 `close()` 总返回成功，外层就可能按成功收尾。

这只是把错误正确传给原有事务机制，并没有让默认 VacuousTrx 获得完整回滚能力。多行修改中途失败后，是否能恢复前面已修改的行，仍取决于事务实现。

### 5.5 所有修改文件的简要导航

下表覆盖 UPDATE 原提交的 20 个代码文件。关键内容已经在第 4 节和本节前半部分解释；枚举、名称注册和头文件声明只需知道它们如何接通流程。

| 文件 | 修改内容与原因 |
| --- | --- |
| `src/observer/sql/stmt/stmt.cpp` | 增加 `SCF_UPDATE → UpdateStmt::create` 分发，让解析结果进入语句检查 |
| `src/observer/sql/stmt/update_stmt.h` | 保存表、目标字段、新值和条件；补充语句类型、访问接口和析构声明 |
| `src/observer/sql/stmt/update_stmt.cpp` | 实现表/列/赋值/条件检查，构造对象并释放其拥有的过滤条件 |
| `src/observer/sql/operator/logical_operator.h` | 增加 UPDATE 逻辑类型，让计划框架识别更新节点 |
| `src/observer/sql/operator/logical_operator.cpp` | UPDATE 选择逐行执行路径；CHUNK 会话也不尝试生成未实现的批量更新算子 |
| `src/observer/sql/operator/update_logical_operator.h` | 新增逻辑更新节点，保存表、字段、新值；方法较短，直接定义在头文件中 |
| `src/observer/sql/optimizer/logical_plan_generator.h` | 增加 UpdateStmt 前向声明和 UPDATE 计划生成方法声明 |
| `src/observer/sql/optimizer/logical_plan_generator.cpp` | 接入 UPDATE，组装更新、过滤、可写取表节点 |
| `src/observer/sql/operator/physical_operator.h` | 增加 UPDATE 物理类型，标识真正执行更新的节点 |
| `src/observer/sql/operator/physical_operator.cpp` | 提供显示名称 UPDATE，供计划展示和日志使用 |
| `src/observer/sql/optimizer/physical_plan_generator.h` | 声明 UpdateLogicalOperator 及其物理计划生成接口 |
| `src/observer/sql/optimizer/physical_plan_generator.cpp` | 递归生成扫描/过滤子计划，并创建 UpdatePhysicalOperator |
| `src/observer/sql/operator/update_physical_operator.h` | 定义更新参数、open/next/close 接口和真实结果状态 |
| `src/observer/sql/operator/update_physical_operator.cpp` | 先复制所有匹配旧行并关闭扫描，再逐行构造和替换新行 |
| `src/observer/storage/table/table.h` | 声明 make_update_record，供更新算子调用 |
| `src/observer/storage/table/table.cpp` | 实现复制整行、清空目标字段、写入新值 |
| `src/observer/storage/trx/vacuous_trx.h` | 将返回未实现的 update_record 改成方法声明 |
| `src/observer/storage/trx/vacuous_trx.cpp` | 默认模式先插入新行再删除旧行，复用索引维护并处理当前替换行的错误清理 |
| `src/observer/storage/trx/mvcc_trx.h` | 将返回未实现的 update_record 改成方法声明 |
| `src/observer/storage/trx/mvcc_trx.cpp` | 组合已有删除旧版本、插入新版本的事务操作 |

没有新增单独的 UPDATE 命令执行器；它沿用 INSERT、DELETE 使用的执行计划与物理算子路径。也没有新增 `SCF_UPDATE` 或 `StmtType::UPDATE`，这些标签原来已经存在。

## 6. 哪些文件没有改，却被 UPDATE 复用了

本题的重要工作之一是接通已有模块。理解这些调用，可以看清为什么没有修改 B+ 树和 SQL 解析器也能完成题目。

### 6.1 `parse_defs.h`、`yacc_sql.y`：语法原本已经支持

文件：`src/observer/sql/parser/parse_defs.h`、`src/observer/sql/parser/yacc_sql.y`。它们不是 UPDATE 提交的修改文件。

已有解析结构保存：

```cpp
struct UpdateSqlNode
{
  string relation_name;
  string attribute_name;
  Value value;
  vector<ConditionSqlNode> conditions;
};
```

已有语法的关键形状为：

```text
UPDATE ID SET ID EQ value where
```

它只提供一个目标列和一个值，本次按照这个范围补执行能力，没有扩展语法为赋值列表或任意表达式。

解析层保存的是名称和常量；语句检查层绑定真实对象；执行层才修改记录。三个阶段各自完成不同工作。

### 6.2 `filter_stmt.cpp`：WHERE 的字段绑定和条件表达式

文件：`src/observer/sql/stmt/filter_stmt.cpp`。UPDATE 调用已有 `FilterStmt::create`，没有为 UPDATE 单独重写一个条件解析器。

例如：

```sql
UPDATE t SET age = 21 WHERE id = 2 AND name = 'Bob';
```

语句层会把条件交给已有过滤模块；逻辑计划生成器再把绑定后的条件转成比较表达式和 AND 组合。后续扫描或过滤节点负责逐行求真/假。

WHERE 中涉及不同类型时，由已有条件比较和类型转换规则处理。赋值类型检查与条件类型检查是两个位置，不能因为 SET 的值合法就跳过 WHERE 的校验。

### 6.3 `heap_table_engine.cpp`：真正维护行数据与普通索引

文件：`src/observer/storage/table/heap_table_engine.cpp`。本次没有改它的代码。

默认模式中的调用链为：

```text
UpdatePhysicalOperator::execute
  → VacuousTrx::update_record
      → insert_record
          → Table::insert_record
              → HeapTableEngine::insert_record
                  → 插入数据记录，得到新 RID
                  → insert_entry_of_indexes
      → delete_record
          → Table::delete_record
              → HeapTableEngine::delete_record
                  → 对每个索引执行 delete_entry
                  → 删除旧 RID 对应的数据记录
```

**修改索引列的例子：** 假设原来的行是 `(id=3, age=20)`，RID 为 `(页1, 槽2)`。更新为 id=9 后，可能分配新 RID `(页1, 槽3)`：

| 时刻 | 记录 | id 索引中的关系 |
| --- | --- | --- |
| 更新前 | 旧 RID 保存 `(3,20)` | `3 → 旧 RID` |
| 插入新行后 | 新旧记录都暂时存在 | `3 → 旧 RID`、`9 → 新 RID` |
| 删除旧行后 | 新 RID 保存 `(9,20)` | `9 → 新 RID` |

这些页号和槽号只是示例，实际地址由记录管理器分配。

**只修改非索引列，为什么仍维护索引？**

若把 age 改成 21，而 id 仍是 3，行替换后关系应从 `3 → 旧 RID` 改成 `3 → 新 RID`。索引键没有变，地址仍然需要变，所以不能简单判断“没改索引列，就不碰索引”。

本次让已有 INSERT/DELETE 处理表上所有索引，正好覆盖这件事。

### 6.4 MVCC 下保留旧索引条目不等于索引错误

默认 VacuousTrx 路径物理删除旧条目；MVCC 路径保留旧版本，所以旧索引条目可以继续存在。

索引扫描取得 RID 后，已有 `IndexScanPhysicalOperator::next` 会通过事务的 `visit_record` 判断这个版本是否可见。当前事务不应看见的旧版本会被过滤。

因此这两种模式下，“更新完索引内部还留不留旧条目”的答案可能不同；用户层应看到的是满足条件且对当前事务可见的新结果。

### 6.5 `Record` 与对象生命周期：谁负责保存和释放内存

文件：`src/observer/storage/record/record.h`，同样没有在本题中修改。

`Record` 可以借用数据页里的字节，也可以通过 `copy_data` 拥有自己的字节。新增算子刻意使用后一种，让收集到的行在扫描关闭后继续有效。

| 对象 | 内容来源及生命周期 |
| --- | --- |
| `Table *`、`FieldMeta *` | 借用当前数据库已有表及元数据，本次 UPDATE 不删除它们 |
| `UpdateStmt::filter_stmt_` | 语句对象负责释放 |
| 逻辑/物理节点中的 `Value` | 以成员形式持有值，各节点有自己的 Value 对象 |
| 计划的子节点 | 通过 `unique_ptr` 转移给父节点，随计划释放 |
| `records` 中的旧行 | 显式复制字节；局部列表销毁时随 Record 释放 |
| 每次循环的 `new_record` | 拥有组装后的新行字节；存储接口读取后由局部对象释放 |

这里“释放新行的内存”不等于“删除已写入数据库的新行”：存储层已经把字节写进自己的数据页，局部对象只负责那份用于传参的内存副本。

### 6.6 `SqlResult` 和通信层：为什么客户端收到 SUCCESS 或 FAILURE

文件：`src/observer/sql/executor/sql_result.cpp`、`src/observer/net/plain_communicator.cpp`，本次没有修改它们。

已有结果处理先调用 `SqlResult::open()`；它启动当前事务，再调用物理算子的 `open(trx)`。UPDATE 在这里实际执行修改。

UPDATE 没有要输出的结果列，`next()` 返回 `RECORD_EOF`，通信层按照已有非查询结果路径返回状态。语句检查或执行失败时，普通文本协议通常显示 `FAILURE`；带解析错误说明时，可能显示更具体的状态文本。

`SqlResult::close()` 在自动事务模式下会读取算子关闭状态：成功就沿已有流程提交，失败就沿已有流程回滚。这正是新增 `result_` 并让 `UpdatePhysicalOperator::close()` 返回它的原因。

### 6.7 为什么没有修改 CMakeLists.txt

已有 `src/observer/CMakeLists.txt` 使用 `FILE(GLOB_RECURSE ALL_SRC *.cpp *.c)` 收集源码。重新运行 CMake 后，新增的 `update_physical_operator.cpp` 会进入构建列表。

所以本题没有手动往 CMake 文件里追加源文件，但新增文件后需要重新配置构建目录。只依赖旧的构建列表，可能没有把新实现编进程序。

## 7. 错误处理和能力边界

### 7.1 错误会在哪个阶段被发现

| 输入或情况 | 发现位置 | 结果 |
| --- | --- | --- |
| 表不存在 | `UpdateStmt::create` 查表 | 返回表不存在错误 |
| SET 目标列不存在或不可见 | `UpdateStmt::create` 查列 | 返回字段不存在错误 |
| 赋值类型不能转换 | `Value::cast_to` | 将转换错误传回调用方 |
| WHERE 引用了不存在的列 | 已有 `FilterStmt::create` | 创建条件失败 |
| WHERE 的比较不能完成类型处理 | 已有条件/计划生成逻辑 | 返回对应错误 |
| 扫描中途失败 | `UpdatePhysicalOperator::execute` | 关闭扫描，不开始后续修改阶段 |
| 构造新行失败 | `Table::make_update_record` | 停止更新，向外返回错误 |
| 存储或事务更新失败 | `Trx::update_record` | 停止更新，通过 `result_` 传播失败 |
| 没有匹配行 | 空的旧行列表 | 正常成功，数据不变 |

表中“停止更新”不等于“所有已经修改的行都自动恢复”。恢复能力取决于当前事务实现；默认 VacuousTrx 不提供整条多行语句回滚保证。

### 7.2 与其他题目和后续提交的边界

本题没有新增多字段赋值、表达式赋值、子查询赋值、NULL 或唯一索引约束，也没有适配其他存储引擎的 UPDATE。

实现接入已有普通计划生成路径，并没有补全 Cascade 优化器中所有 UPDATE 转换规则。不能根据公共 `OpType` 中存在 UPDATE 标签，就认为所有优化器模式都已经适配。

DATE 转换直接复用已有类型系统；VECTOR、大对象、复杂 JOIN 等后续功能需要以各自代码和验证结果为准。本次文档也不把后续功能自动继承所有更新能力当成已验证结论。

默认模式以行替换实现更新，没有保持 RID 不变的承诺。SQL 用户通常按列值查询数据，不应依赖这种内部物理地址。

### 7.3 正确性与性能上的取舍

先收集行、再关闭扫描、再修改，让实现容易避开扫描期间修改索引和页内存失效的问题，符合本题的实现规模。

代价是需要额外保存匹配旧行，并且行替换会对数据与索引执行增删操作。它不是对所有更新场景都最省内存或写入最少的算法；本题没有要求在此基础上实现生产数据库级的大批量更新优化。

## 8. 测试与手动复现实验

### 8.1 编译与启动独立实验环境

在已经配置好 MiniOB 依赖的 Linux 环境中，从项目根目录运行：

```bash
cmake -S . -B build
cmake --build build --target observer obclient -j 4
```

以已有 Docker 容器中的项目路径 `/root/miniob` 为例，可以在独立工作目录启动数据库，避免复用项目原来的数据目录：

```bash
mkdir -p /tmp/miniob-update-learning
cd /tmp/miniob-update-learning
/root/miniob/build/bin/observer -f /root/miniob/etc/observer.ini -s /tmp/miniob-update-learning.sock
```

在另一个终端连接同一个 socket：

```bash
/root/miniob/build/bin/obclient -s /tmp/miniob-update-learning.sock
```

这里的 socket 是本机客户端与服务端通信的入口，不是表数据文件。若你的项目不在 `/root/miniob`，把以上绝对路径换成实际路径。首次实验使用一个未放入其他数据的工作目录；重复运行前要确认同名测试表是否已经存在。

### 8.2 UPDATE 实现时已经完成的验证

开发时使用临时脚本启动真正的 observer，通过 SQL 协议请求数据库，再检查 SELECT 返回的数据。测试数据放在独立临时目录，不使用原来的数据库文件。验证完成后已清理临时测试脚本和日志，最终仅保留功能代码和本文档。

以下是原 UPDATE 实现时的验证记录，不是本次扩写文档后新运行的测试：observer 编译成功，开启 AddressSanitizer，运行时关闭全局泄漏扫描；以下四组共通过 **942 条 SQL 检查**（包含测试数据插入和 SELECT 结果断言）：

| 配置 | SQL 检查数 |
| --- | --- |
| 默认 VacuousTrx + 内存日志 | 230 |
| 默认 VacuousTrx + 磁盘日志 | 230 |
| MvccTrx + 内存日志 | 241 |
| MvccTrx + 磁盘日志 | 241 |

覆盖有条件/无条件更新、多行更新、AND 条件、空表、零匹配、索引查旧值和新值、重复索引键、更新为原值、字符串变短、FLOAT 赋值转换、无效表/列/类型、CHUNK 会话中的 UPDATE，以及 180 行跨数据页更新。MVCC 组另检查了无索引表上的显式提交和回滚。

验证边界：上述通过结果针对运行中的 SQL。额外重启检查遇到了现有恢复流程的读错误；大批量操作后执行 DDL 触发同步也出现超时。后者用只包含原有 INSERT/DELETE、完全不执行 UPDATE 的对照程序同样能复现。因此没有把重启恢复列为本次通过项，也没有扩展修改恢复或同步模块。默认 VacuousTrx 本身不具备完整事务回滚保证；本次不增加这种保证。

### 8.3 基本更新与索引查询

在客户端按顺序执行下面的 SQL，观察每次 UPDATE 后 SELECT 的结果：

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

### 8.4 接着验证 AND、零匹配和同值更新

以下 SQL 接着上一节执行，此时 t 表有三行：`(1,21,Alice)`、`(9,21,B)`、`(9,21,Carol)`。

```sql
UPDATE t SET name = 'X' WHERE id = 9 AND name = 'B';
SELECT * FROM t WHERE id = 9;
```

应看到两行，name 分别为 `X`、`Carol`。满足 id 条件但不满足 name 条件的 Carol 那行没有被修改。

```sql
UPDATE t SET id = 10 WHERE id = 9;
SELECT * FROM t WHERE id = 9;
SELECT * FROM t WHERE id = 10;
```

`id = 9` 应没有行，`id = 10` 应有两行。这一步刻意使用 id 索引定位行，同时修改 id 本身，用来理解“先收集再更新”的意义。

```sql
UPDATE t SET id = 10 WHERE id = 10;
SELECT * FROM t WHERE id = 10;
```

更新为原值后仍应恰好有两行，不能变成四行，也不能丢行。

```sql
UPDATE t SET age = 99 WHERE id = 999;
SELECT * FROM t;
```

UPDATE 返回成功，三行数据保持原样。它不会因为 WHERE 没找到行就执行 INSERT。

### 8.5 验证错误输入与空表

```sql
UPDATE missing_table SET age = 1;
UPDATE t SET missing_column = 1;
UPDATE t SET age = 1 WHERE missing_column = 1;
UPDATE t SET age = 'bad' WHERE id = 1;
UPDATE t SET age = 'bad' WHERE id = 999;
```

以上都应失败。最后一条故意使用没有匹配行的条件，验证赋值检查是在扫描前完成的。

```sql
CREATE TABLE empty_update(id INT);
UPDATE empty_update SET id = 7;
SELECT * FROM empty_update;
```

空表更新应成功，SELECT 只有表头而没有数据行。

### 8.6 验证赋值转换和执行模式

```sql
CREATE TABLE update_float(id INT, score FLOAT);
INSERT INTO update_float VALUES (1, 1.5);
UPDATE update_float SET score = 2 WHERE id = 1;
SELECT * FROM update_float;
```

应看到 id 为 1、score 数值为 2 的一行。SET 中的整数通过已有类型系统转换为 FLOAT，查询输出的具体小数格式沿用现有格式化规则。

```sql
SET execution_mode = 'chunk_iterator';
UPDATE t SET age = 22 WHERE id = 1;
SET execution_mode = 'tuple_iterator';
SELECT * FROM t WHERE id = 1;
```

应看到 `1 | 22 | Alice`。会话设为 CHUNK 时 UPDATE 仍使用逐行更新路径；SELECT 前改回逐行模式，便于把观察重点放在本题更新行为上。

以上是可以自行运行的实验步骤。此次只更新讲解文档，没有重新执行编译或 SQL 回归，也没有恢复已经删除的临时测试脚本。

## 9. 把完整过程再串一次：SQL 中的每一部分去了哪里

假设当前有一行 `(id=2, age=20, name=Bob)`，执行：

```sql
UPDATE t SET age = 21 WHERE id = 2;
```

| 阶段 | 接收什么 | 交出什么 |
| --- | --- | --- |
| 已有语法解析 | SQL 文本 | `UpdateSqlNode`：t、age、21、id=2 |
| `Stmt::create_stmt` | `SCF_UPDATE` 解析结果 | 调用 `UpdateStmt::create` |
| `UpdateStmt::create` | 表名、列名、常量和条件 | 已绑定的 Table、age 元数据、整数 Value、FilterStmt |
| 逻辑计划生成 | `UpdateStmt` | UPDATE 与取行、过滤节点组成的树 |
| 物理计划生成 | 逻辑树 | UPDATE 与具体扫描/过滤算子组成的树 |
| 更新算子收集阶段 | 子算子交来的匹配行 | 旧行字节副本及旧 RID 的列表 |
| 新行构造 | 旧行、age 元数据、新值 21 | `(2,21,Bob)` 的字节副本 |
| 默认事务更新 | 新行和旧行 | 插入新行及索引，删除旧行及索引 |
| 已有结果处理 | 算子执行状态 | 完成事务收尾，向客户端返回状态 |

这里 id 和 name 最终保持不变，是因为新行构造先复制了整条旧行，再只覆盖 age。WHERE id=2 决定“修改哪些行”，SET age=21 决定“这些行中修改哪一列、改成什么”，两者由不同组件负责。

如果改用 MVCC 事务，前面的语句、计划、收集和新行构造不变，进入 `trx->update_record` 后才切换到旧版本标记与新版本插入。物理更新算子通过统一接口调用事务，避免把两种事务细节混在扫描循环里。

## 10. 关键函数与建议阅读顺序

| 函数或接口 | 文件 | 阅读时关注什么 |
| --- | --- | --- |
| `Stmt::create_stmt` | `src/observer/sql/stmt/stmt.cpp` | UPDATE 是否进入专门的语句创建分支 |
| `UpdateStmt::create` | `src/observer/sql/stmt/update_stmt.cpp` | 表、列、类型、条件依次怎样检查 |
| `UpdateStmt` 构造/析构 | 同上 | 值怎样保存，谁释放 FilterStmt |
| `LogicalPlanGenerator::create` | `src/observer/sql/optimizer/logical_plan_generator.cpp` | 语句对象怎样进入 UPDATE 逻辑计划生成 |
| `create_plan(UpdateStmt *, ...)` | 同上 | 没有 WHERE 和有 WHERE 的树有什么不同 |
| `LogicalOperator::can_generate_vectorized_operator` | `src/observer/sql/operator/logical_operator.cpp` | 为什么 CHUNK 会话也能走逐行 UPDATE |
| `PhysicalPlanGenerator::create` | `src/observer/sql/optimizer/physical_plan_generator.cpp` | 如何识别 UPDATE 逻辑节点 |
| `create_plan(UpdateLogicalOperator &, ...)` | 同上 | 如何递归生成子物理算子并接上 UPDATE |
| `UpdatePhysicalOperator::open / execute` | `src/observer/sql/operator/update_physical_operator.cpp` | 先收集、后修改的两个循环和错误处理 |
| `UpdatePhysicalOperator::next / close` | `src/observer/sql/operator/update_physical_operator.h` | 没有结果行与保留真实状态的区别 |
| `Table::make_update_record` | `src/observer/storage/table/table.cpp` | 如何只替换一列的原始字节 |
| `VacuousTrx::update_record` | `src/observer/storage/trx/vacuous_trx.cpp` | 默认模式为何先插入、后删除 |
| `MvccTrx::update_record` | `src/observer/storage/trx/mvcc_trx.cpp` | 为什么交给已有版本增删方法 |
| 已有 `HeapTableEngine::insert_record / delete_record` | `src/observer/storage/table/heap_table_engine.cpp` | 表数据与索引条目在哪一起维护 |
| 已有 `SqlResult::open / close` | `src/observer/sql/executor/sql_result.cpp` | 更新何时执行，状态怎样影响事务收尾 |

第一次阅读时，可以先沿 `stmt.cpp → update_stmt.cpp → logical_plan_generator.cpp → physical_plan_generator.cpp → update_physical_operator.cpp → table.cpp → vacuous_trx.cpp` 走通默认模式，再回头阅读每个头文件的成员和接口，最后理解 MVCC 的版本差异。
