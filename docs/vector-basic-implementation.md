# vector-basic：向量类型基础功能实现讲解

本文面向只有 SQL 基础、尚未接触数据库内核的读者，说明本题实现了什么、代码如何配合，以及为什么这样设计。

## 1. 本题实现范围

本次实现围绕题目要求展开：

- 创建包含 `VECTOR(N)` 列的表。
- 通过 `STRING_TO_VECTOR` 将字符串转换为向量并插入。
- 通过 `VECTOR_TO_STRING` 将向量转换为题目要求的科学计数法字符串。
- 支持 `DISTANCE(v1, v2, 'COSINE'/'EUCLIDEAN'/'DOT')`。
- 支持示例中的 `AS` 别名，以及没有 `FROM` 的常量计算查询。

向量元素使用四字节浮点数 `float` 保存。当前实现中，`VECTOR(N)` 的 `N` 是允许保存的最大维度，实际维度随数据保存；省略 `N` 时默认上限为 2048，最大上限为 16383。计算两个向量的距离时，二者的实际维度必须相同。

没有增加向量索引、近似最近邻搜索等能力。

## 2. 先用 SQL 理解效果

```sql
CREATE TABLE test (id INT, c1 VECTOR(3));

INSERT INTO test VALUES (
    1,
    STRING_TO_VECTOR('[1, 2, 3]')
);

SELECT
    id,
    VECTOR_TO_STRING(c1) AS vec_str,
    DISTANCE(c1, STRING_TO_VECTOR('[2, 3, 4]'), 'EUCLIDEAN') AS dist
FROM test;
```

结果：

```text
id | vec_str | dist
1 | [1.00000e+00,2.00000e+00,3.00000e+00] | 1.73
```

也可以直接计算常量，不需要先建表：

```sql
SELECT DISTANCE(
    STRING_TO_VECTOR('[1, 2, 3]'),
    STRING_TO_VECTOR('[2, 3, 4]'),
    'COSINE'
) AS dist_cosine;
```

`'[1, 2, 3]'` 本身只是字符串。`STRING_TO_VECTOR` 的返回值才是数据库内部可以参与向量计算的值。向量可以理解为有顺序的数字数组，`[1,2,3]` 和 `[3,2,1]` 不相等。

## 3. 一条 SQL 在数据库中经过哪些步骤

数据库收到的是 SQL 文本，不能直接拿它访问磁盘。它需要逐步把文本转换成可以执行的操作。

```text
SQL 文本
   ↓
词法、语法解析：识别关键字、函数、参数和语句结构
   ↓
字段绑定和检查：确认 c1 属于哪张表，检查函数参数类型
   ↓
表达式计算：转换字符串，或者根据当前行计算距离
   ↓
读写记录：保存或读取向量数据
   ↓
格式化并返回结果
```

原来的代码虽然已有 `VECTORS` 类型入口，但只有类型名称并不足以完成上述过程，因此需要补齐解析、内存表示、函数执行和存储这些环节。

## 4. SQL 解析：让数据库读懂函数调用

相关文件：

- `src/observer/sql/parser/lex_sql.l`
- `src/observer/sql/parser/yacc_sql.y`
- `src/observer/sql/parser/parse_defs.h`

词法解析负责把 SQL 文本切成小片段，例如 `SELECT`、函数名、括号和字符串。这里增加了 `STRING_TO_VECTOR`、`VECTOR_TO_STRING`、`DISTANCE` 和 `AS` 的识别。

语法解析负责判断这些片段如何组成语句。例如，它把 `DISTANCE(c1, ..., 'DOT')` 组织成一个函数表达式，并保存三个参数。

本次还补充了两个题目示例所必需的语法：

1. `SELECT 表达式 AS 名称`：将名称作为结果列标题。
2. `SELECT 表达式`：没有 `FROM` 时，复用数据库已有的计算语句执行流程。

### 为什么 INSERT 也要改

原来的插入语句主要保存直接给出的值，例如整数 `1` 和字符串 `'hello'`。但题目中的第二项是函数：

```sql
INSERT INTO test VALUES (1, STRING_TO_VECTOR('[1,2,3]'));
```

必须先执行函数，才能得到真正要插入的向量。所以将插入节点中的值列表改成表达式列表，在创建 `InsertStmt` 时调用 `try_get_value` 求值，再把得到的值交给原有插入流程。

`InsertStmt` 自己持有求值后的 `vector<Value>`，避免结果依赖临时对象的生命周期。

## 5. Value：内存中的一个值怎样保存向量

相关文件：`src/observer/common/value.h`、`src/observer/common/value.cpp`。

`Value` 可以理解为数据库内部用来装“一个值”的盒子。它原来能装整数、浮点数和字符串，现在也能装浮点数组。

例如 `STRING_TO_VECTOR('[1,2,3]')` 得到的盒子包含：

```text
类型：VECTORS
内容：1.0、2.0、3.0 三个 float
字节数：3 × 4 = 12
```

增加的主要接口：

| 接口 | 用途 |
| --- | --- |
| `set_vector` | 分配内存并复制向量元素 |
| `vector_dimension` | 根据字节数得到元素个数 |
| `vector_at` | 读取指定位置的元素 |

向量复制使用深拷贝：分配另一块内存，再复制数组内容。如果只复制内存地址，两个对象会共用一份数据，其中一个释放数据后，另一个就可能访问已经失效的内存。

相应地，`reset` 和析构流程也负责释放向量内存，`data`、`set_data`、`set_value` 等通用入口也补上了向量处理。

`vector_at` 使用 `memcpy` 取出一个浮点数，避免假定字节数据一定满足浮点数的内存对齐要求。

## 6. 字符串与向量转换

新增实现：`src/observer/common/type/vector_type.cpp`。

### STRING_TO_VECTOR

输入检查顺序是：

```text
读取左方括号 → 读取数字 → 读取逗号或右方括号 → 检查结尾
```

支持负数、小数、科学计数法和空白，例如：

```sql
STRING_TO_VECTOR(' [ 1.05, -17.8, 3.2e1 ] ')
```

空向量、缺少数字、错误分隔符、尾随字符、非有限数值和超过维度上限的输入会报错。例如 `[]`、`[1,]`、`[1 2]`、`[nan]`、`[inf]` 都不能成为有效向量。

这些检查放在转换入口，可以阻止错误数据进入计算和存储环节。

### VECTOR_TO_STRING

输出使用：

```cpp
std::scientific << std::setprecision(5)
```

因此数字 `1` 输出为 `1.00000e+00`，向量输出为：

```text
[1.00000e+00,2.00000e+00,3.00000e+00]
```

这里的五位小数是显示格式，磁盘里保存的仍然是二进制 `float`。输入和输出流固定使用经典区域设置，保证小数点格式一致。

## 7. 表达式、绑定和距离计算

相关文件：

- `src/observer/sql/expr/expression.h`
- `src/observer/sql/expr/vector_function_expr.cpp`
- `src/observer/sql/expr/expression_iterator.cpp`
- `src/observer/sql/parser/expression_binder.cpp`

“表达式”就是执行后能得到一个值的东西，`1+2`、字段 `c1`、函数 `DISTANCE(...)` 都属于表达式。

新增 `VectorFunctionExpr`，用同一个表达式类处理本题的三个函数，检查参数数量和类型，并计算结果。

对于全是常量的函数，`try_get_value` 可以直接求值。对于包含字段的函数，`get_value` 根据当前查询行提取参数，再执行计算。两种情况复用同一套计算逻辑。

### 为什么需要字段绑定

解析器读到 `c1` 时，只知道这是一个名字。绑定器需要根据 `FROM test` 和表结构，把它对应到 `test` 表的 `c1` 字段。

因此，绑定器必须能够进入向量函数内部，逐个绑定参数；表达式遍历器也必须认识这个新表达式，才能继续访问它的子表达式。别名需要在绑定、复制过程中保留，才能正确显示结果列标题。

### 三种计算方法

设两个向量为 `a`、`b`，第 `i` 个元素分别为 `a[i]`、`b[i]`。

| 方法 | 公式 | 含义 |
| --- | --- | --- |
| EUCLIDEAN | `sqrt(Σ(a[i] - b[i])²)` | 欧氏距离，可以理解为两点间的直线距离 |
| DOT | `Σ(a[i] × b[i])` | 点积，按题目约定直接返回点积 |
| COSINE | `1 - dot(a,b) / (norm(a) × norm(b))` | 余弦距离，比较两个向量的方向差异 |

其中 `norm(a) = sqrt(Σa[i]²)`，表示向量的长度。

以 `a=[1,2,3]`、`b=[2,3,4]` 为例：

```text
EUCLIDEAN = sqrt(1 + 1 + 1) = sqrt(3) ≈ 1.73205
DOT       = 1×2 + 2×3 + 3×4 = 20
```

余弦距离中，方向相同时结果接近 0，方向相反时接近 2。点积本身不是通常意义上的距离，不能统一按“数值越小越接近”理解。

计算前检查实际维度是否相同。计算余弦距离时拒绝零向量，因为它的长度为 0，会导致除以零。累加过程使用 `double`，最后转换成 MiniOB 已有的浮点结果类型，以减少中间误差；余弦相似度还限制在 `[-1,1]`，避免舍入误差越界。

## 8. 磁盘存储：为什么使用 .lob 文件

相关文件：

- `src/observer/storage/record/vector_record.h`
- `src/observer/storage/table/table.cpp`、`table.h`
- `src/observer/storage/field/field_meta.cpp`、`field_meta.h`
- `src/observer/storage/table/table_meta.cpp`
- `src/observer/storage/record/lob_handler.cpp`、`lob_handler.h`
- `src/observer/sql/expr/tuple.h`

数据库把表中的一行称为一条“记录”，普通记录存放在固定大小的数据页里。一个 16383 维向量仅元素就需要 `16383×4=65532` 字节，直接放进普通记录会遇到大小限制。

因此采用以下布局：

```text
普通表记录：
[id][向量描述信息：offset、length][其他字段]

表对应的 .lob 文件：
[向量的实际 float 数组][下一个向量的数组]……
```

描述信息定义为：

```cpp
struct VectorRecord
{
  int64_t offset; // 向量在 .lob 文件中的起始位置
  int64_t length; // 向量占用的字节数
};
```

可以把普通记录里的描述信息理解成一张“取件条”，向量本身放在另一个文件中。

### 插入过程

1. 确认值确实是向量，且实际维度没有超过列的上限。
2. 将浮点数组追加到 `.lob` 文件，得到文件位置。
3. 把位置和字节数写入普通记录。

### 查询过程

1. 从普通记录取出描述信息。
2. 检查位置、长度和维度是否合法。
3. 到 `.lob` 文件读取浮点数组，恢复成 `Value`。
4. 将该值交给 `DISTANCE` 或 `VECTOR_TO_STRING`。

`Tuple` 可以理解为查询当前处理的一行，`RowTuple::cell_at` 增加了向量读取分支。表打开时也会打开对应的 `.lob` 文件，因此重启后能够重新读取。

字段元数据同时区分了两种长度：`vector_dimension()` 表示列的维度上限，`len()` 表示字段在普通记录中占用的空间。后者对向量返回描述信息的大小，而不是浮点数组的大小。后续字段的位置也按这个大小计算，避免向量后面的普通列被错误读取。

LOB 读写增加了互斥保护，避免同时操作文件位置产生冲突；读取时还检查实际读取字节数，防止把不完整的数据当作有效向量。

本实现使用追加方式保存向量数据，没有实现 LOB 空间回收、向量索引等扩展能力。测试验证了普通停止并重启后的读取，没有对突然断电的恢复行为作保证。

## 9. 其他配套修改

- `create_table_stmt.cpp`、`field_meta.cpp`：检查向量列的维度上限，拒绝零维和超出范围的定义。
- `plain_communicator.cpp`：将 SQL 接收上限从 8KB 调整为 1MB，让大向量的字符串能够完整进入数据库。
- `expression.cpp`、`filter_stmt.cpp`：约束向量比较，只允许向量之间进行 `=`、`<>`。向量没有在本题中定义大小顺序，不能直接使用 `<`、`>` 等运算。
- `yacc_sql.y` 的条件常量解析：支持 `WHERE c1 = STRING_TO_VECTOR(...)` 这类向量常量条件。

## 10. 用一条 INSERT 串起全部代码

```sql
INSERT INTO test VALUES (1, STRING_TO_VECTOR('[1,2,3]'));
```

完整过程可以概括为：

```text
解析器识别 INSERT 和函数调用
   ↓
InsertStmt 对函数表达式求值
   ↓
VectorFunctionExpr 调用 VectorType 解析字符串
   ↓
得到拥有 3 个 float 的 Value
   ↓
Table 检查列类型和维度上限
   ↓
LOB 写入浮点数组，返回文件位置
   ↓
普通记录保存 id 和向量描述信息
```

后续查询时反过来读取描述信息、恢复向量，再计算或格式化输出。这就是本题从 SQL 到持久化数据的完整链路。

## 11. 验证结果与清理

在 Docker 容器 `obtest` 中编译成功，并在 `vacuous`、`mvcc` 两种事务模式下通过共 130 项 SQL 检查，覆盖：

- 建表、插入、查询及多个向量列与普通列混合存储。
- 三种距离、函数嵌套、别名及无 `FROM` 的计算查询。
- 科学计数法输出、向量相等和不等比较。
- 非法字符串、错误类型、错误参数数量、维度不匹配及非法距离方法。
- 16383 维向量的写入、距离计算和完整字符串输出。
- 停止并重启数据库后的向量读取，以及删表后的 LOB 文件清理。

测试使用的临时脚本和临时数据库目录均已删除，功能提交中仅保留实现代码。

功能代码提交：`7bde928c0ee12384125c792ef819cea8076e9c12`，提交说明为 `feat: implement basic vector database support`。之后又补充了计算查询的错误返回处理：常量表达式会在发送结果表头前求值，维度不匹配时返回普通 `FAILURE`，不会因为执行错误关闭客户端连接。
