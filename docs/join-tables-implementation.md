# INNER JOIN 实现详解

> 本文面向只有 SQL 基础、还不熟悉数据库内核的读者，按 DATE 讲解文档的方式说明：每个关键文件改了什么、为什么这样改，以及这些修改怎样串起一次查询。
>
> 重点讲解语法、条件安排、类型转换和逐条执行四部分。已有模块只说明衔接关系，语法声明和内存释放等次要修改简要带过。

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

### 3.1 阅读代码前需要认识的几个对象

| 对象 | 可以先这样理解 |
| --- | --- |
| token | SQL 中识别出的符号，例如 INNER、表名、等号 |
| SqlNode | 语法解析结果，保存表名、字段名、条件等信息 |
| Stmt | 经过表、字段检查后的语句对象 |
| Value | 执行期携带一个值及其类型的对象 |
| Expression | 字段、常量、比较、类型转换等表达式 |
| Predicate | 结果为真或假的条件，例如 `t.id=t1.id` |
| Tuple | 执行过程中可以读取字段的一条记录，不一定是一份独立拷贝 |

本题处理的是查询过程中的记录组合，不需要把连接结果写成一张新表，所以没有修改存储格式、表元数据或 B+ 树结构。

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

其中 ConditionSqlNode 也是原来的结构，能表示“左边是什么、运算符是什么、右边是什么”。例如 `t.id=t1.id` 保存两个字段引用和 EQUAL_TO；`t1.age>=18` 保存一个字段、一个常量和 GREAT_EQUAL。因此 ON 无需再增加一套条件对象。

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

其中连续 JOIN 的关键解析动作是：

```cpp
| joined_table INNER JOIN relation ON join_condition_list {
  $$ = $1;
  $$->relations.emplace_back($4);
  $$->conditions.insert($$->conditions.end(), $6->begin(), $6->end());
  delete $6;
}
```

`$1`、`$4`、`$6` 是 Bison 对规则右侧位置的编号，`$$` 表示合并后的结果：

```text
joined_table INNER JOIN relation ON join_condition_list
     $1        $2   $3     $4    $5          $6
```

这段代码复用左侧连接组，再追加新表和本次 ON 的条件。连续连接时，结果逐步变成：

```text
表 [t]          条件 []
表 [t,t1]       条件 [t.id=t1.id]
表 [t,t1,t2]    条件 [t.id=t1.id, t1.id=t2.id]
```

逗号规则则把右侧连接组的 relations 和 conditions 一起追加，避免只接入表、丢失右侧 ON 条件。

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

这一步对应 select_stmt 中的关键修改：

```cpp
if ($4 != nullptr) {
  $$->selection.relations.swap($4->relations);
  $$->selection.conditions.swap($4->conditions);
  delete $4;
}

if ($5 != nullptr) {
  auto &conditions = $$->selection.conditions;
  conditions.insert(conditions.end(), $5->begin(), $5->end());
  delete $5;
}
```

SELECT 的语法是 `SELECT expression_list FROM rel_list where group_by`，因此 `$4` 是 FROM 的结果，`$5` 是 WHERE 的结果。

先接收 ON 条件，再追加 WHERE 条件。原来只有 WHERE 时，可以直接交换整个条件列表；现在如果仍用 WHERE 替换列表，就会丢掉 ON 条件，导致不匹配的行也参与输出。

原代码的 `SelectStmt::create` 会根据这些条件调用 `FilterStmt::create`，检查条件中的表和字段，构造内部过滤对象。随后，逻辑计划生成器把这些对象转换成比较表达式，并处理已有的隐式类型转换规则。

这样 ON 和 WHERE 使用同一套字段解析、比较和类型转换能力，不需要再实现一份 ON 专用逻辑。后续补充的字符串与数值比较也在这条共同路径中处理。

这里只利用本题内连接的性质；不能直接把同样的合并方式用于 LEFT JOIN 等外连接，因为它们还有补 NULL 行等语义。

### 6.5 临时对象如何释放

`FromSqlNode` 和条件列表在语法动作中临时创建。正常解析时，代码通过 `swap` 转移列表，或通过 `insert` 追加内容，随后删除不再需要的临时对象。

语法文件中还为新的 FROM 类型声明了 `%destructor`；解析出错、Bison 丢弃对应符号时，可以释放该临时结构。这避免新增的解析结构在错误路径上被遗留。

同一文件还增加 INNER/JOIN token，并用 `%union`、`%type` 告诉 Bison 新规则使用 FromSqlNode 指针。这些是让新结构接入解析器所需的配套声明。

`lex_sql.cpp`、`yacc_sql.cpp` 等由 CMake 调用 Flex/Bison 根据 `.l`、`.y` 自动生成，且已被 Git 忽略，所以修改规则源码即可。

### 6.6 已有语义分析怎样接上这次改动

下面的代码没有因本题修改，只需理解它们的衔接关系：

| 文件/函数 | 在本题中的作用 |
| --- | --- |
| `src/observer/sql/stmt/select_stmt.cpp`：SelectStmt::create | 按 relations 查找真实 Table 对象，收集表并绑定 SELECT 字段 |
| `src/observer/sql/stmt/filter_stmt.cpp`：FilterStmt::create | 把 conditions 中每个条件转换成 FilterUnit |
| 同文件：get_table_and_field | 通过表名、字段名找到真实表与字段元数据 |

语法阶段的 `t.id` 还只是名称，绑定后得到的 Field 则知道自己属于哪个 Table。后面的计划生成器才能判断条件依赖哪些表。不存在的表和字段会在检查阶段返回错误。

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

### 7.3 先拆开原来的总 AND 条件

修改函数：`LogicalPlanGenerator::create_plan(SelectStmt *, ...)`。

原流程已经能从 FilterStmt 构造一个包含 AND 的过滤算子，其中的比较也已处理类型转换。本次先复用它，再取出里面的条件：

```cpp
vector<unique_ptr<Expression>> predicates;
if (predicate_oper) {
  auto *conjunction = static_cast<ConjunctionExpr *>(
      predicate_oper->expressions().front().get());
  predicates.swap(conjunction->children());
  predicate_oper.reset();
}
```

`predicates` 是尚未安排执行位置的条件列表。这样不需要再构造一份比较，也不会丢失已有 CastExpr。

### 7.4 apply_predicates 怎样判断条件能否执行

新增的局部函数接收当前逻辑算子和当前可用的表列表：

```cpp
auto apply_predicates = [&](unique_ptr<LogicalOperator> &oper,
                            const vector<Table *> &available_tables) {
  // 检查并安排当前能执行的条件
};
```

`oper` 是当前计划子树，`available_tables` 表示这棵子树能提供哪些表的数据。它遍历每个条件的表达式，检查字段所属的 Table 是否全部可用。

检查字段的关键代码为：

```cpp
if (expr->type() == ExprType::FIELD) {
  const Table *table = static_cast<FieldExpr *>(expr.get())->field().table();
  bool found = false;
  for (Table *available_table : available_tables) {
    found = found || table == available_table;
  }
  available = available && found;
}
return ExpressionIterator::iterate_child_expr(*expr, check_fields);
```

例如 `t.id=t1.id`：只有 t 可用时不能判断；t、t1 都可用时才能判断。代码比较的是已绑定的真实 Table 对象，不是靠字段名称猜测。

能执行的条件加入 ready，并从待安排列表移走，避免后续重复挂载；缺表的条件保留给后面的连接步骤。ready 中的条件重新组成 AND，包装当前计划：

```cpp
if (!ready.empty()) {
  auto conjunction = make_unique<ConjunctionExpr>(ConjunctionExpr::Type::AND, ready);
  auto predicate = make_unique<PredicateLogicalOperator>(std::move(conjunction));
  predicate->add_child(std::move(oper));
  oper = std::move(predicate);
}
```

包装后变成“过滤算子 → 原来的子树”。原子树提供记录，过滤算子负责判断。这里的 `std::move` 转移对象所有权，不是复制整张表或全部中间记录。

### 7.5 为什么每接入一张表调用两次

构建每张表的扫描计划时，先调用：

```cpp
apply_predicates(table_get_oper, vector<Table *>{table});
```

这次只有一张表可用，因此能安排 `t2.score>60` 等单表条件。

把新表接入已有连接子树后，再调用：

```cpp
joined_tables.push_back(table);
apply_predicates(table_oper, joined_tables);
```

这次可以读取已经连接的全部表，因此能安排 `t.id=t1.id` 等跨表条件。连接子树按表的书写顺序逐步构建：先连接前两表，再把它们的结果与第三表连接。

已有的 `predicate_pushdown_rewriter.cpp` 还可以把扫描上方的单表过滤移进 TABLE_GET，最终由扫描算子执行。因此实际 EXPLAIN 有时直接显示 TABLE_SCAN，而不单独显示该单表过滤节点。

### 7.6 为什么要进入类型转换表达式内部

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

## 8. 字符串与数值比较：为什么 '16a' 不能返回 FAILURE

涉及两个文件：`src/observer/sql/optimizer/logical_plan_generator.cpp` 和 `src/observer/common/type/char_type.cpp`。

### 8.1 根因：计划阶段没有可用的类型转换方向

后续反馈暴露了初次验证遗漏的混合类型场景：

```sql
INSERT INTO join_table_1 VALUES (4, '16a');
SELECT * FROM join_table_1 INNER JOIN join_table_2
ON join_table_1.name < join_table_2.age
AND join_table_1.id = join_table_2.id;
```

当 name 为 CHAR、age 为 INT 或 FLOAT 时，原类型转换代价表没有提供可用的转换方向，计划生成器会在执行连接前返回不支持，客户端显示 FAILURE。

这属于本题需要覆盖的 ON 比较，需要同时修改“计划选择什么转换”和“类型层怎样执行转换”。只改其中一处不够。

### 8.2 logical_plan_generator.cpp：比较前统一为数值类型

修改函数：`LogicalPlanGenerator::create_plan(FilterStmt *, ...)`。注意它与上一节安排条件位置的 SelectStmt 重载是同文件中的两个函数。

从 FilterUnit 构造左右 FieldExpr/ValueExpr 后，增加：

```cpp
if (left->value_type() == AttrType::CHARS &&
    (right->value_type() == AttrType::INTS || right->value_type() == AttrType::FLOATS)) {
  left = make_unique<CastExpr>(std::move(left), AttrType::FLOATS);
} else if (right->value_type() == AttrType::CHARS &&
           (left->value_type() == AttrType::INTS || left->value_type() == AttrType::FLOATS)) {
  right = make_unique<CastExpr>(std::move(right), AttrType::FLOATS);
}
```

字符串一侧被包上一层转 FLOAT 的表达式；另一侧如果是 INT，则继续使用原有规则提升为 FLOAT。最终变成 `FLOAT(name 的数字前缀) < FLOAT(age)`。

选择 FLOAT 是为了保留字符串中的小数。例如 `'16.5a' > 16` 应成立，若先转 INT 变成 16，就会得出错误结果。比较的左右两侧都做了对应处理，所以 `name<age` 和 `age>name` 使用同样的数值解释。

### 8.3 char_type.cpp：真正执行 CHAR → FLOAT

修改函数：`CharType::cast_to`，增加 FLOATS 分支：

```cpp
case AttrType::FLOATS: {
  result.set_float(val.get_float());
  return RC::SUCCESS;
}
```

CastExpr 只是声明需要转换；读取一条记录时，真正的调用关系是：

```text
CastExpr::get_value
  → 从子表达式读取原值
  → Value::cast_to
  → 根据源类型 CHAR 调用 CharType::cast_to
  → Value::get_float 解析数字前缀
  → 得到 FLOAT 类型的 Value
```

Value::get_float 原本已经存在，本次没有修改它，而是接上其已有转换能力。它使用 `stof` 读取数字前缀，解析异常时按已有逻辑返回 0。因此：

| 原字符串 | 参与数值比较的值 |
| --- | --- |
| `'16a'` | 16 |
| `'16.5a'` | 16.5 |
| `'-2.5suffix'` | -2.5 |
| `'  +3.5x'` | 3.5 |
| `'2e1x'` | 20 |
| `'abc'` 或空字符串 | 0 |

两侧均为 CHAR 时继续按字符串规则比较，例如 `'10' < '2'` 仍是字符串的字典序比较。日期转换和原有数字之间的转换规则保持原样。

没有修改 CharType::cast_cost：这里直接在比较计划中为 CHAR 与 INT/FLOAT 选择转 FLOAT，随后沿用原有数值转换规则。类型层新增的是实际转换分支，不是把所有字符串比较都改为数值比较。

转换只影响表达式求值，不改写原表，因此结果中仍然输出原字符串 `'16a'`。

例如为 id=4 插入 `age=17` 的匹配记录后，上述查询应输出包含 `(4, '16a', 4, 17)` 的结果；若 age=16，查询应正常执行并过滤掉这一组合。

### 8.4 与条件提前执行、索引的关系

转换后字段藏在 CastExpr 内部，所以前面的条件依赖检查必须递归进入子表达式，确保对应表可读后才求值。

带索引时，也不能把原始字段值直接当作转换后的 FLOAT 值查找。现有索引选择逻辑要求相应的字段/常量表达式形态，出现转换表达式时可能采用表扫描，逐行算出正确比较值。本题没有增加“转换后的表达式索引”。

### 8.5 性能边界

本次执行仍然使用已有的嵌套循环连接。两张表分别有 M、N 行时，最坏仍可能需要 `M × N` 次组合比较。

提前过滤主要减少不匹配记录参与后续连接的次数，避免多表连接的条件全部拖到最后执行。本次没有增加哈希连接、连接顺序搜索等额外功能。

## 9. 执行器：每次 next() 返回一条组合记录

修改文件：`src/observer/sql/operator/nested_loop_join_physical_operator.cpp`。

### 9.1 嵌套循环连接是什么

最直观的连接方式类似下面的双重循环：

```text
对于左侧的每条记录：
    从头遍历右侧的记录：
        产生一条左右组合记录
        交给上层检查条件或输出
```

左侧也可以是前面两张表的连接结果，因此相同的两输入连接组件可以逐层组成多表连接。

### 9.2 next() 的返回值代表什么

MiniOB 的执行算子按需逐条提供记录。上层调用 `next()`，可以理解为“请准备下一条记录”。

| 返回值 | 含义 |
| --- | --- |
| `RC::SUCCESS` | 已准备好一条记录，可以通过 current_tuple() 取得 |
| `RC::RECORD_EOF` | 已没有更多记录 |
| 其他错误码 | 执行发生错误 |

上层过滤算子取得记录后判断条件：满足时向上返回，不满足时继续向下请求下一条。

### 9.3 原代码的问题和修正

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

例如左侧有 A、B 两行，右侧有 X、Y 两行：

```text
第 1 次 next()：准备 A+X，返回 SUCCESS
第 2 次 next()：准备 A+Y，返回 SUCCESS
第 3 次 next()：右侧结束，推进左侧并重开右侧，准备 B+X
第 4 次 next()：准备 B+Y，返回 SUCCESS
第 5 次 next()：左侧也结束，返回 RECORD_EOF
```

`round_done_` 标记右侧一轮结束，left_next 推进左侧，right_next 负责必要时重新打开右侧。这些已有逻辑保持原样；本次补的是成功组合的返回位置。

执行过程中逐条传递组合记录，无需一次性保存所有组合。

### 9.4 已有组件怎样完成“组合、过滤、输出”

下面的文件没有因本题修改，只是复用：

| 文件 | 作用 |
| --- | --- |
| `src/observer/sql/expr/tuple.h` | JoinedTuple 保存当前左右 Tuple 引用，按表名、字段名从相应一侧取值；左侧也可以是前面两表的 JoinedTuple |
| `src/observer/sql/operator/predicate_physical_operator.cpp` | 反复调用下层 next()，条件为真就向上返回，否则继续索取下一条组合 |
| `src/observer/sql/optimizer/physical_plan_generator.cpp` | 把逻辑 JOIN 转换为 NestedLoopJoinPhysicalOperator，把逻辑过滤转换成对应物理过滤器 |

因此跨表条件由连接上方的过滤器检查，连接器负责产生组合。这两个组件配合完成本题内连接，不需要把 ON 判断另写进连接器。

## 10. 用题目示例串起整个过程

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

## 11. 验证结果与清理

以下是实现与后续修复时在 `obtest` Docker 开发容器中完成的验证记录。本次只扩写讲解文档，没有重新运行这些测试。

### 11.1 编译与语法检查

- 使用 `cmake --build build --target observer -j4` 编译，成功生成 observer。
- 使用 Bison 检查新增语法，没有移进/归约冲突或归约/归约冲突。
- `git diff --check` 通过。

### 11.2 SQL 集成验证

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

### 11.3 执行计划验证

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

### 11.4 清理情况

本次创建的临时测试脚本、编译日志、临时语法生成文件和独立测试实例的数据目录均已删除。

仓库原有的测试文件保持原样，保留本题功能源码修改和本文档。

## 12. 实现范围与源码阅读顺序

本次围绕 INNER JOIN、AND 条件和已有嵌套循环实现，没有增加外连接、表别名、自连接别名支持、OR、复杂 ON 表达式、哈希连接或连接顺序搜索。

连接组最终整理成表列表和条件列表，没有保留完整 JOIN 树，也没有增加严格的逐个 ON 名称作用域校验。本文混用示例都引用对应连接组可见的字段；不能把这个简化实现理解成完整 SQL JOIN 语义。

CHAR/数值比较使用仓库已有的 FLOAT 精度和比较规则，没有增加高精度数值类型。

建议阅读顺序：

1. lex_sql.l、parse_defs.h：认识关键字和新增中间结构。
2. yacc_sql.y 的 joined_table、join_condition_list、select_stmt：看清表与条件如何收集、合并。
3. logical_plan_generator.cpp 的两个 create_plan：SelectStmt 版本安排过滤位置，FilterStmt 版本构造比较与转换。
4. char_type.cpp 的 FLOATS 分支：理解字符串怎样得到数值。
5. nested_loop_join_physical_operator.cpp 的 next()：理解记录怎样逐条返回。

其他已有模块按正文的衔接表查阅即可。
