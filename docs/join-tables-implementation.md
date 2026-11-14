# MiniOB join-tables：INNER JOIN 实现与原理讲解

本文面向了解一些 SQL、还不熟悉数据库内核的读者，说明本次 join-tables 题目修改了什么、为什么这样实现，以及 SQL 如何经过这些代码得到查询结果。

## 1. 本次实现的范围

题目要求实现 `INNER JOIN`，支持多表连接、一个 ON 中的多个条件，以及显式内连接与逗号形式的隐式内连接混用。

本次实现支持以下写法：

```sql
-- 两表连接
SELECT * FROM t INNER JOIN t1 ON t.id = t1.id;

-- 连续连接多张表
SELECT * FROM t
INNER JOIN t1 ON t.id = t1.id
INNER JOIN t2 ON t1.id = t2.id;

-- 一个 ON 中的多个 AND 条件，同时使用 WHERE
SELECT * FROM t
INNER JOIN t1 ON t.id = t1.id AND t1.score >= 60
WHERE t.age > 18;

-- 显式连接与逗号连接混用：题目中的示例
SELECT * FROM t INNER JOIN t1 ON t.id = t1.id, t2
WHERE t2.id = t.id;

-- 逗号出现在显式连接之前
SELECT * FROM t, t1 INNER JOIN t2 ON t1.id = t2.id
WHERE t.id = t1.id;
```

这些示例假定对应表和字段已经存在。ON 中的条件复用原有比较语法，可以比较字段和字段、字段和常量、常量和字段，以及常量和常量；多个条件通过 `AND` 连接。

本次复用已有的字段解析、比较运算、类型转换、表扫描和嵌套循环连接能力，只修改完成本题需要的部分。

## 2. 先理解 INNER JOIN 做了什么

假设有两张表：

`student`：

| id | name |
| --- | --- |
| 1 | Alice |
| 2 | Bob |
| 3 | Carol |

`score`：

| student_id | points |
| --- | --- |
| 1 | 90 |
| 2 | 80 |
| 2 | 85 |

执行：

```sql
SELECT student.name, score.points
FROM student INNER JOIN score ON student.id = score.student_id;
```

结果包含以下记录，未指定排序时不保证显示顺序：

| name | points |
| --- | --- |
| Alice | 90 |
| Bob | 80 |
| Bob | 85 |

可以把它理解为：从两张表中选择记录组合，保留满足 ON 条件的组合。

Carol 没有匹配的成绩记录，因此不输出；Bob 有两条匹配记录，因此输出两行。内连接需要保留所有匹配组合，不能找到一个匹配后就跳过剩余匹配，也不能擅自去重。

对于本题中的内连接，下面的查询具有相同的筛选要求：

```sql
SELECT student.name, score.points
FROM student, score
WHERE student.id = score.student_id;
```

这就是本次实现可以复用原有多表查询能力的依据：收集所有参与查询的表，再收集所有必须同时满足的条件。

## 3. SQL 在 MiniOB 中经过哪些步骤

一次查询大致经过下面的流程：

```text
SQL 字符串
  ↓
词法分析：识别 SELECT、INNER、JOIN、表名、字段名、等号等符号
  ↓
语法分析：识别哪些表参与连接、ON 和 WHERE 中有哪些条件
  ↓
语义分析：检查表和字段是否存在，把字段名关联到真实的表和字段
  ↓
逻辑计划：安排读取、连接、过滤和输出的顺序
  ↓
物理计划与执行：选择具体算子，逐条读取和处理记录
  ↓
返回查询结果
```

这里的“算子”就是负责某项工作的执行组件，例如表扫描算子读取记录，连接算子组合记录，过滤算子判断条件，投影算子输出 SELECT 指定的字段。

“逻辑计划”描述要做什么，例如连接两份数据；“物理计划”决定具体怎么做，例如使用嵌套循环逐对组合记录。

## 4. 修改文件一览

以下路径相对于仓库根目录：

| 文件 | 修改内容 | 目的 |
| --- | --- | --- |
| `src/observer/sql/parser/lex_sql.l` | 增加 INNER、JOIN 关键字 | 让词法分析器识别连接关键字 |
| `src/observer/sql/parser/parse_defs.h` | 增加 FromSqlNode | 在解析 FROM 时收集表名和 ON 条件 |
| `src/observer/sql/parser/yacc_sql.y` | 扩展连接语法，合并 ON 与 WHERE 条件 | 支持连续连接、多条件及逗号混用 |
| `src/observer/sql/optimizer/logical_plan_generator.cpp` | 按条件依赖安排过滤位置，统一字符串与数值比较的类型 | 减少无效中间组合，支持混合类型的 ON 条件 |
| `src/observer/common/type/char_type.cpp` | 补充字符串转浮点数的转换入口 | 让字符串的数字前缀参与数值比较 |
| `src/observer/sql/operator/nested_loop_join_physical_operator.cpp` | 成功取得一对记录后立即返回 | 让连接结果逐条交给上层处理 |

除上述六个功能源码文件外，本说明文档也随本次提交保存。

## 5. 词法分析：识别 INNER 和 JOIN

修改文件：`src/observer/sql/parser/lex_sql.l`。

词法分析负责把字符串拆成具有含义的符号，这些符号通常称为 token。

例如：

```sql
t INNER JOIN t1 ON t.id = t1.id
```

需要识别出：表名 t、关键字 INNER、关键字 JOIN、表名 t1、关键字 ON、字段引用和等号。

新增规则为：

```cpp
INNER                                   RETURN_TOKEN(INNER);
JOIN                                    RETURN_TOKEN(JOIN);
```

它们放在通用标识符规则之前，避免这两个单词被当成普通名称。原词法分析器已经设置大小写不敏感，因此 `inner join`、`INNER JOIN` 和混合大小写都能识别。

ON 关键字原本已经存在，本次直接复用。

## 6. 语法分析：收集表和 ON 条件

### 6.1 为什么增加 FromSqlNode

修改文件：`src/observer/sql/parser/parse_defs.h`。

原来的 FROM 解析结果只需要保存表名列表。支持显式连接后，解析 FROM 的过程中还会遇到 ON 条件，因此增加临时结构：

```cpp
struct FromSqlNode
{
  vector<string>           relations;
  vector<ConditionSqlNode> conditions;
};
```

`relations` 保存按 SQL 书写顺序出现的表名，`conditions` 保存各个 ON 中的比较条件。

它是语法解析期间的中间结果，不保存表中的实际记录。表名最后进入已有的 `SelectSqlNode::relations`，ON 条件进入已有的 `SelectSqlNode::conditions`。

保留表的书写顺序，也有助于保持 `SELECT *` 展开各表字段时的列顺序。

### 6.2 如何支持连续连接和逗号混用

修改文件：`src/observer/sql/parser/yacc_sql.y`。

新增 INNER、JOIN token，并把 FROM 的语法拆成两个层次。用接近中文的写法表示：

```text
表列表 rel_list：
    一组连接
    或者：表列表 , 一组连接

一组连接 joined_table：
    一张表
    或者：一组连接 INNER JOIN 一张表 ON 条件列表
```

对应的主要语法规则是：

```text
rel_list:
    joined_table
  | rel_list COMMA joined_table

joined_table:
    relation
  | joined_table INNER JOIN relation ON join_condition_list
```

第二条 joined_table 规则可以反复使用：先解析 t，再把 t1 加进来，再把 t2 加进来，因此可以连续连接多张表。

rel_list 则允许用逗号连接多个这样的组，因此既能解析：

```sql
FROM t INNER JOIN t1 ON t.id = t1.id, t2
```

也能解析：

```sql
FROM t, t1 INNER JOIN t2 ON t1.id = t2.id
```

每次合并都会追加对应的表名和 ON 条件。

### 6.3 如何支持一个 ON 中的多个条件

新增独立的 `join_condition_list`：

```text
join_condition_list:
    condition
  | join_condition_list AND condition
```

它要求 ON 后至少有一个完整条件，并允许继续追加 AND 条件，例如：

```sql
ON t.id = t1.id AND t.age > t1.age AND t1.score >= 60
```

这里复用已有的 `condition`，所以比较运算符的处理方式与原有 WHERE 一致。

这个新列表没有空规则，因此 `ON;` 或 `ON t.id=t1.id AND;` 会返回语法错误。原有 WHERE 语法没有因为本次扩展而重写。

### 6.4 为什么把 ON 和 WHERE 条件合并

对于本题中合法的内连接查询，ON 条件和 WHERE 条件都必须满足，才能输出对应的记录组合。它们通过 AND 连接，因此可以统一交给已有的过滤流程。

例如：

```sql
SELECT *
FROM t INNER JOIN t1 ON t.id = t1.id AND t1.score >= 60, t2
WHERE t2.id = t.id;
```

最终收集为：

```text
表：t、t1、t2

条件：
  t.id = t1.id
  t1.score >= 60
  t2.id = t.id
```

原代码的 `SelectStmt::create` 会根据这些条件调用 `FilterStmt::create`，检查条件中的表和字段，构造内部过滤对象。随后，逻辑计划生成器把这些对象转换成比较表达式，并处理已有的隐式类型转换规则。

这样 ON 和 WHERE 使用同一套字段解析、比较和类型转换能力，不需要再实现一份 ON 专用逻辑。后续补充的字符串与数值比较也在这条共同路径中处理。

### 6.5 临时对象如何释放

`FromSqlNode` 和条件列表在语法动作中临时创建。正常解析时，代码通过 `swap` 转移列表，或通过 `insert` 追加内容，随后删除不再需要的临时对象。

语法文件中还为新的 FROM 类型声明了 `%destructor`；解析出错、Bison 丢弃对应符号时，可以释放该临时结构。这避免新增的解析结构在错误路径上被遗留。

## 7. 执行计划：让条件尽早过滤

修改文件：`src/observer/sql/optimizer/logical_plan_generator.cpp`。

### 7.1 为什么只扩展语法还不够

如果先枚举所有表的组合，最后才检查条件，多表查询可能产生大量无用工作。

一张表有 100 行，两张表的全部组合有：

```text
100 × 100 = 10,000 个组合
```

三张表则有：

```text
100 × 100 × 100 = 1,000,000 个组合
```

六张表的全部组合达到 `100^6`，也就是一万亿个。每张表的每一行都与其他表每一行组合的操作，叫作“笛卡尔积”。

很多组合在前两张表连接时就已经不满足条件，继续把它们与后面的表连接，会浪费大量时间。这里的问题是枚举工作量，不代表原执行器会把全部组合一次性存进内存。

### 7.2 条件应该放在哪里

以这条查询为例：

```sql
SELECT *
FROM t
INNER JOIN t1 ON t.id = t1.id
INNER JOIN t2 ON t1.id = t2.id
WHERE t2.score > 60;
```

三个条件依赖不同的数据：

| 条件 | 需要哪些表 | 最早可以执行的位置 |
| --- | --- | --- |
| `t2.score > 60` | t2 | 扫描 t2 时 |
| `t.id = t1.id` | t、t1 | t 与 t1 连接后 |
| `t1.id = t2.id` | t1、t2 | 再连接 t2 后 |

安排后的计划可以理解为：

```text
输出 SELECT 的字段
└─过滤 t1.id = t2.id
  └─连接
    ├─过滤 t.id = t1.id
    │ └─连接
    │   ├─读取 t
    │   └─读取 t1
    └─读取并过滤 t2.score > 60 的 t2
```

不匹配的 t、t1 组合会在参与下一次连接之前被丢弃；t2 中分数不达标的记录也不必参与连接。这种把条件安排到更早执行位置的做法通常称为“谓词下推”，这里的“谓词”就是产生真或假结果的条件。

### 7.3 apply_predicates 的具体工作

代码先复用原流程生成比较表达式，保留其中已经完成的类型转换，然后把 AND 中的各个比较取出来，作为待安排的条件列表。

新增的 `apply_predicates` 接收当前逻辑算子和当前可用的表列表。它对每个待安排条件执行以下操作：

1. 遍历条件表达式，找到其中引用的字段。
2. 检查这些字段所属的表是否全部可用。
3. 如果全部可用，把条件移到当前算子上方的过滤算子中。
4. 如果有表尚不可用，保留条件，等待后面的连接步骤。

构建每张表的扫描计划时，先使用只有该表的可用列表调用一次，因此单表条件可以提前处理。

每连接进一张新表后，再使用已经连接的全部表调用一次，因此跨表条件可以在所需数据齐全时立即处理。

条件被安排后会从待安排列表移走，不会在后续步骤重复挂载。

### 7.4 为什么要进入类型转换表达式内部

例如：

```sql
ON t.int_id = t1.float_id
```

原有类型转换逻辑可能把一侧包在 CastExpr 中，内部结构类似：

```text
比较表达式
├─类型转换
│ └─t.int_id
└─t1.float_id
```

如果只检查最外层节点，就可能遗漏里面的字段，进而错误判断条件依赖哪些表。

因此代码使用已有的 `ExpressionIterator` 递归访问子表达式，连同类型转换内部的字段一起检查。ON 和 WHERE 共用转换路径，整数和浮点数的转换沿用原有规则；字符串与数值比较的补充规则如下。

#### 字符串与数值比较：为什么 '16a' 不能返回 FAILURE

后续反馈暴露了初次验证遗漏的混合类型场景：

```sql
INSERT INTO join_table_1 VALUES (4, '16a');
SELECT * FROM join_table_1 INNER JOIN join_table_2
ON join_table_1.name < join_table_2.age
AND join_table_1.id = join_table_2.id;
```

当 name 为 CHAR、age 为 INT 或 FLOAT 时，原类型转换代价表没有提供可用的转换方向，计划生成器会在执行连接前返回不支持，客户端显示 FAILURE。

这属于本题需要覆盖的 ON 比较，因此补充以下处理：

1. 生成比较表达式时，发现一侧为 CHAR、另一侧为 INT/FLOAT，就为字符串一侧添加转 FLOAT 的 CastExpr。
2. 如果另一侧是 INT，继续使用已有规则把 INT 提升为 FLOAT，使两侧按数值比较。
3. 在 CharType::cast_to 中补充 FLOAT 分支，调用已有的 Value::get_float() 读取数字前缀。

已有的 Value::get_float() 能处理数字前缀；没有有效数字前缀时返回 0。因此：

| 原字符串 | 参与数值比较的值 |
| --- | --- |
| `'16a'` | 16 |
| `'16.5a'` | 16.5 |
| `'-2.5suffix'` | -2.5 |
| `'  +3.5x'` | 3.5 |
| `'2e1x'` | 20 |
| `'abc'` 或空字符串 | 0 |

选择 FLOAT 是为了保留字符串中的小数。例如 `'16.5a' > 16` 应成立，若先转 INT 变成 16，就会得出错误结果。字符串出现在比较左侧或右侧都按同样的规则处理。

两侧均为 CHAR 时继续按字符串规则比较，例如 `'10' < '2'` 仍是字符串的字典序比较。日期转换和原有数字之间的转换规则保持原样。

本次只在比较计划中为 CHAR 与 INT/FLOAT 的组合选择这个转换，不增加字符串转 INT 等其他转换，也不把所有字符串比较一律改为数值比较。

例如为 id=4 插入 `age=17` 的匹配记录后，上述查询应输出包含 `(4, '16a', 4, 17)` 的结果；若 age=16，查询应正常执行并过滤掉这一组合。

### 7.5 性能边界

本次执行仍然使用已有的嵌套循环连接。两张表分别有 M、N 行时，最坏仍可能需要 `M × N` 次组合比较。

提前过滤主要减少不匹配记录参与后续连接的次数，避免多表连接的条件全部拖到最后执行。本次没有增加哈希连接、连接顺序搜索等额外功能。

## 8. 执行器：每次 next() 返回一条组合记录

修改文件：`src/observer/sql/operator/nested_loop_join_physical_operator.cpp`。

### 8.1 嵌套循环连接是什么

最直观的连接方式类似下面的双重循环：

```text
对于左侧的每条记录：
    从头遍历右侧的记录：
        产生一条左右组合记录
        交给上层检查条件或输出
```

左侧也可以是前面两张表的连接结果，因此相同的两输入连接组件可以逐层组成多表连接。

### 8.2 next() 的返回值代表什么

MiniOB 的执行算子按需逐条提供记录。上层调用 `next()`，可以理解为“请准备下一条记录”。

| 返回值 | 含义 |
| --- | --- |
| `RC::SUCCESS` | 已准备好一条记录，可以通过 current_tuple() 取得 |
| `RC::RECORD_EOF` | 已没有更多记录 |
| 其他错误码 | 执行发生错误 |

上层过滤算子取得记录后判断条件：满足时向上返回，不满足时继续向下请求下一条。

### 8.3 原代码的问题和修正

原来的连接执行器在 `right_next()` 成功取得右侧记录后，仍继续内部循环，没有把当前左右组合返回给上层。它会继续遍历，最后到达结束状态，导致已有多表执行框架无法正常输出组合记录。

本次增加：

```cpp
rc = right_next();
if (rc == RC::SUCCESS) {
  // 每次 next() 只产出一对记录，交给上层过滤或输出。
  return rc;
}
```

此时左、右记录已经被设置到 `joined_tuple_`，因此返回成功后，上层能通过 `current_tuple()` 读取这条组合记录。

下一次调用 `next()` 时继续遍历右侧；右侧遍历完后，再推进左侧并重新扫描右侧。这既能保留一对多匹配，也能支持逐层构成多表连接。

执行过程中逐条传递组合记录，无需一次性保存所有组合。

## 9. 用题目示例串起整个过程

```sql
SELECT * FROM t INNER JOIN t1 ON t.id = t1.id, t2
WHERE t2.id = t.id;
```

执行过程如下：

1. 词法分析器识别 INNER、JOIN、ON、WHERE 等关键字。
2. 语法分析器收集表名 `[t, t1, t2]`。
3. 收集 ON 条件 `t.id = t1.id`。
4. 追加 WHERE 条件 `t2.id = t.id`。
5. 原有语义分析检查三张表和条件中的字段，构造过滤对象。
6. 逻辑计划生成器构造比较表达式，并沿用已有类型转换处理。
7. 建立 t 与 t1 的连接，并立即检查 `t.id = t1.id`。
8. 将通过检查的组合与 t2 连接，再检查 `t2.id = t.id`。
9. 连接执行器通过 next() 逐条向上提供记录。
10. 投影算子输出 SELECT * 指定的各表字段。

最终留下的每条记录，都满足两个连接条件。

## 10. 验证结果与清理

验证在本机已有的 `obtest` Docker 开发容器中完成。

### 10.1 编译与语法检查

- 使用 `cmake --build build --target observer -j4` 编译，成功生成 observer。
- 使用 Bison 检查新增语法，没有移进/归约冲突或归约/归约冲突。
- `git diff --check` 通过。

### 10.2 SQL 集成验证

使用临时 Python 脚本启动独立的 observer 实例，通过 Unix socket 发送 SQL，并在 SQLite 中运行同样的合法查询，对照结果。

比较时忽略未指定 ORDER BY 的行顺序，保留每条结果出现的次数，避免漏掉重复匹配或错误去重的问题。

最初通过的验证共执行 **809 条语句**，其中 **141 个 SELECT 的结果与 SQLite 一致**。这批验证没有覆盖字符串与数值的混合比较，后续根据反馈补上了该场景的实现与验证。

覆盖内容包括：

- 仓库已有的 `test/case/test/primary-join-tables.test`：631 条语句，包含 13 个 SELECT 结果比较。
- 每张表 100 行的六表连接。
- 两表和多表连接，以及一个 ON 中的多个 AND 条件。
- 显式连接、逗号连接和多个连接组的混用。
- 空表、无匹配记录、一对多匹配和重复记录。
- 等值和非等值比较、常量条件、反向的常量与字段比较。
- 整数与浮点数字段的连接和已有类型转换。
- 大小写混合关键字。
- 连接后使用已有 SUM 聚合的分组查询。
- 100 条固定随机种子生成的三表连接查询。
- 缺少 ON 条件、AND 后缺少条件、表或字段不存在等错误输入，以及错误之后继续执行正常查询。

验证期间额外尝试 COUNT 时，确认原有聚合执行代码只实现 SUM，COUNT 会触发未实现分支。该问题不属于本题范围，未修改聚合实现；最终分组验证使用已有的 SUM。

针对后续字符串与数值比较的修复，另外执行了 **774 条语句，核对 107 个 SELECT 的结果，全部通过**，其中包含原有整套 join 题目用例。补充覆盖：

- 先在修复前确认反馈中的查询返回 FAILURE，再用同样的建表与数据验证修复后输出 `4 | 16a | 4 | 17`。
- `'16a'` 与 16、17 的等值及不等值边界，带小数、符号、前导空白和科学计数法的数字前缀。
- 非数字字符串和空字符串常量按 0 参与数值比较。
- CHAR 与 INT/FLOAT 分别位于比较左侧或右侧，以及全部已有比较运算符。
- 多条件 ON、三表连接、逗号连接、WHERE 和字符串常量的比较。
- 加索引后的混合类型比较，避免用原始字段值错误地代替转换后的值查找。
- 纯字符串比较和原有整数/浮点数比较的回归检查。

SQLite 原生的字符串与数字混合比较规则不同，补充验证中的混合类型结果使用独立的数字前缀转换及逐对比较计算预期值；普通连接及纯字符串比较继续与 SQLite 对照。没有把 SQLite 的原生混合类型行为当成本题的预期。

### 10.3 执行计划验证

对于三表查询，EXPLAIN 返回了类似下面的实际计划：

```text
PROJECT
└─PREDICATE
  └─NESTED_LOOP_JOIN
    ├─PREDICATE
    │ └─NESTED_LOOP_JOIN
    │   ├─TABLE_SCAN(ja)
    │   └─TABLE_SCAN(jb)
    └─PREDICATE
      └─TABLE_SCAN(jc)
```

它表明：ja、jb 的条件在两表连接后执行；jc 的单表条件在扫描阶段执行；最后再检查需要第三张表参与的条件。

### 10.4 清理情况

本次创建的临时测试脚本、编译日志、临时语法生成文件和独立测试实例的数据目录均已删除。

仓库原有的测试文件保持原样，保留本题功能源码修改和本文档。
