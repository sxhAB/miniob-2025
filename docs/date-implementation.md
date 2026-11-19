# DATE 类型实现详解

> 本文面向只有 SQL 基础、没有数据库内核开发经验的读者，说明这道题实现了什么、一条 SQL 在 MiniOB 中怎样走到 DATE 字段、每个文件为什么这样改，以及如何验证。
>
> 实现范围只围绕题目要求：DATE 字段的建表、插入、比较、索引、输出和非法日期校验。没有扩展日期加减、日期聚合等能力。

## 0. 先看最终效果

建表：

```sql
CREATE TABLE t(
    id INT,
    birthday DATE
);
```

插入：

```sql
INSERT INTO t VALUES (1, '2022-10-10');
INSERT INTO t VALUES (2, '2020-1-1');
```

查询：

```sql
SELECT * FROM t;
```

输出：

```text
id | birthday
1  | 2022-10-10
2  | 2020-01-01
```

比较：

```sql
SELECT * FROM t WHERE birthday > '2021-1-1';
```

非法日期：

```sql
INSERT INTO t VALUES (3, '2017-2-29');
```

返回：

```text
FAILURE
```

`2017` 不是闰年，所以 `2017-2-29` 非法。这里的“非法”不是查不到数据，而是整条 SQL 执行失败。

---

## 1. 先理解一条 SQL 在 MiniOB 中怎样执行

数据库收到的是 SQL 文本，不能直接拿文本去读写磁盘。它要把文本逐步转换成可以执行的操作：

```text
SQL 文本
  ↓
词法分析、语法分析
  ↓
生成 Stmt（语句对象）
  ↓
生成逻辑计划 Logical Plan
  ↓
生成物理计划 Physical Plan
  ↓
执行算子 Operator
  ↓
读写 Table / Record / Index
  ↓
格式化结果并返回客户端
```

DATE 类型不是一个独立模块，而是接入这套已有流程。它主要涉及：

- 解析层：认识 `DATE` 关键字；
- 类型层：新增 `AttrType::DATES` 和 `DateType`；
- 值层：让 `Value` 能携带日期；
- 转换层：字符串和日期之间能互相转换；
- 存储层：日期按固定 4 字节写入记录；
- 索引层：B+ 树能用日期键比较大小；
- 输出层：把日期格式化成 `YYYY-MM-DD`。

### 1.1 几个名词

| 名词 | 可以先这样理解 |
| --- | --- |
| `AttrType` | 字段类型的枚举，例如 CHARS、INTS、FLOATS，现在多了 DATES |
| `Value` | 执行期装“一个值”的盒子，带类型和数据 |
| `DataType` | 某一种类型的行为集合：比较、转换、格式化等 |
| `FieldMeta` | 某一列的说明书：列名、类型、长度、在行中的偏移 |
| `TableMeta` | 一张表的说明书：有哪些列、索引、记录大小 |
| `Record` | 一行数据的原始字节 |
| `Stmt` | 一条 SQL 解析并检查后的语句对象 |
| 逻辑计划 | 描述“要做什么”，例如扫描、过滤、投影 |
| 物理计划 | 描述“用什么具体算子做”，例如全表扫描或索引扫描 |
| 谓词 Predicate | WHERE 中的条件，例如 `birthday > '2020-1-1'` |

---

## 2. 核心设计：为什么把 DATE 存成 `YYYYMMDD` 整数

### 2.1 不能直接用 Unix 时间戳

Unix 时间戳通常表示“从 1970-01-01 起经过的秒数”，用 32 位有符号整数保存时，会在 2038 年附近溢出。

题目明确说明：

> date 测试可能超过 2038 年 2 月，也可能小于 1970 年 1 月 1 号。

所以不能用普通的 32 位秒级时间戳。

### 2.2 采用 `YYYYMMDD` 整数

本实现把日期保存成：

```text
2022-10-10  ->  2022 * 10000 + 10 * 100 + 10  =  20221010
2020-01-01  ->  2020 * 10000 +  1 * 100 +  1  =  20200101
```

这样保存有几个直接好处：

1. **仍然是 4 字节**，和 `INT` 一样，可以直接放进定长记录；
2. **整数顺序就是日期顺序**，比较时直接比较整数，不需要先转时间戳；
3. **不受 2038 和 1970 限制**，只要年份在 1~9999 内都能表示；
4. **实现简单**，不需要引入时区、时间库或额外的日期结构。

对比：

| 方案 | 占用 | 1970 年前 | 2038 年后 | 比较方式 |
| --- | --- | --- | --- | --- |
| 32 位 Unix 时间戳 | 4 字节 | 不能直接表示 | 会溢出 | 先转时间戳再比 |
| `YYYYMMDD` 整数 | 4 字节 | 可以表示 | 可以表示 | 直接比整数 |

因此，存储和比较可以概括为：

```cpp
// 存储
date_value = year * 10000 + month * 100 + day;

// 比较：整数大小顺序与日期先后顺序一致
// 20200101 < 20201021
```

---

## 3. 总体改动一览

| 文件 | 作用 |
| --- | --- |
| `src/observer/common/type/attr_type.h` | 增加 `AttrType::DATES` |
| `src/observer/common/type/attr_type.cpp` | 类型名数组增加 `"dates"` |
| `src/observer/common/type/date_type.h` | 新增 `DateType` 类声明 |
| `src/observer/common/type/date_type.cpp` | 日期的解析、比较、转换、格式化 |
| `src/observer/common/type/data_type.cpp` | 把 `DateType` 注册到类型工厂 |
| `src/observer/common/value.h` | `Value` 增加 `set_date` / `get_date` |
| `src/observer/common/value.cpp` | `Value` 支持 DATES 的数据读写 |
| `src/observer/common/type/char_type.cpp` | 字符串隐式转换为 DATE |
| `src/observer/sql/parser/lex_sql.l` | 词法分析识别 `DATE` |
| `src/observer/sql/parser/yacc_sql.y` | 建表语法支持 DATE 类型 |
| `src/observer/sql/optimizer/physical_plan_generator.cpp` | 修复 `<>` 误用等值索引扫描的问题 |

---

## 4. 类型系统：增加 DATES 和 DateType

### 4.1 增加 AttrType::DATES

文件：`src/observer/common/type/attr_type.h`

`AttrType` 是数据库内部表示字段类型的枚举。原来有 CHARS、INTS、FLOATS 等，现在增加：

```cpp
enum class AttrType
{
  UNDEFINED,
  CHARS,
  INTS,
  FLOATS,
  VECTORS,
  BOOLEANS,
  DATES,     // 新增
  MAXTYPE,
};
```

`MAXTYPE` 之前的类型数量决定了类型工厂数组的大小，所以新增类型必须放在 `UNDEFINED` 与 `MAXTYPE` 之间。

### 4.2 增加类型名 `"dates"`

文件：`src/observer/common/type/attr_type.cpp`

```cpp
const char *ATTR_TYPE_NAME[] = {
    "undefined", "chars", "ints", "floats", "vectors", "booleans", "dates"};
```

这个字符串不是随便写的。表的元数据在保存到 JSON 时，会把字段类型写成字符串：

```text
field name=birthday, type=dates, len=4
```

服务端重启后，`FieldMeta::from_json` 会调用 `attr_type_from_string("dates")` 还原成 `AttrType::DATES`。如果名字数组没有同步增加，带 DATE 字段的表重启后就会加载失败。

### 4.3 新增 DateType

文件：`src/observer/common/type/date_type.h`、`src/observer/common/type/date_type.cpp`

`DateType` 继承 `DataType`，负责日期自己的行为：

| 方法 | 作用 |
| --- | --- |
| `compare` | 比较两个日期 |
| `cast_to` | DATE 转成 CHARS 等类型 |
| `set_value_from_str` | 支持从文件导入日期，例如 `LOAD DATA` |
| `to_string` | 格式化成 `YYYY-MM-DD` |
| `parse_date` | 把字符串解析成 `YYYYMMDD` 整数并校验合法性 |

### 4.4 注册到类型工厂

文件：`src/observer/common/type/data_type.cpp`

`DataType::type_instances_` 是一个按 `AttrType` 枚举下标访问的数组：

```cpp
array<unique_ptr<DataType>, static_cast<int>(AttrType::MAXTYPE)> DataType::type_instances_ = {
    make_unique<DataType>(AttrType::UNDEFINED),
    make_unique<CharType>(),
    make_unique<IntegerType>(),
    make_unique<FloatType>(),
    make_unique<VectorType>(),
    make_unique<DataType>(AttrType::BOOLEANS),
    make_unique<DateType>(),   // 新增
};
```

顺序必须和枚举一致：

```text
UNDEFINED -> 0
CHARS     -> 1
INTS      -> 2
FLOATS    -> 3
VECTORS   -> 4
BOOLEANS  -> 5
DATES     -> 6
```

如果 `DateType` 放错位置，`DataType::type_instance(AttrType::DATES)` 会拿到别的类型对象，后续比较和格式化都会出错。

---

## 5. Value：执行期怎样携带一个日期

文件：`src/observer/common/value.h`、`src/observer/common/value.cpp`

`Value` 是执行期真正在算子之间传递的值。它原来能装整数、浮点数、字符串，现在也能装日期。

### 5.1 新增接口

```cpp
void set_date(int val);
int  get_date() const;
```

`set_date` 会把类型设置为 `DATES`，把 `YYYYMMDD` 整数放进 union：

```cpp
void Value::set_date(int val)
{
  reset();
  attr_type_        = AttrType::DATES;
  value_.int_value_ = val;
  length_           = sizeof(val);   // 4 字节
}
```

`get_date` 读取这个整数：

```cpp
int Value::get_date() const
{
  ASSERT(attr_type_ == AttrType::DATES, "attr type is not DATES");
  return value_.int_value_;
}
```

### 5.2 `set_data` 为什么是关键

从记录里读出一个字段时，`RowTuple::cell_at` 会这样做：

```cpp
cell.set_type(field_meta->type());
cell.set_data(record->data() + field_meta->offset(), field_meta->len());
```

`set_data` 根据类型解释原始字节。因此必须增加 DATES 分支：

```cpp
case AttrType::DATES: {
  value_.int_value_ = *(int32_t *)data;
  length_           = length;
} break;
```

如果不加这一段，DATE 字段读出来就是未定义值。

更重要的是，B+ 树的索引键比较也会用 `Value::set_data`：

```cpp
Value left;
left.set_type(attr_type_);
left.set_data(v1, attr_length_);
Value right;
right.set_type(attr_type_);
right.set_data(v2, attr_length_);
return DataType::type_instance(attr_type_)->compare(left, right);
```

所以 `set_data` 同时解决了：

- 从记录中读取 DATE 字段；
- 从索引键中读取 DATE 字段。

### 5.3 其他同步修改

为了保持 `Value` 的行为一致，还增加了：

- `Value::get_int()` 的 DATES 分支；
- `Value::get_float()` 的 DATES 分支；
- `Value::get_boolean()` 的 DATES 分支；
- `Value::set_value()` 的 DATES 分支。

这些接口在类型转换、谓词判断和复制值时可能被调用。

---

## 6. 字符串怎样变成 DATE

### 6.1 SQL 里的日期字面量本来只是字符串

执行：

```sql
INSERT INTO t VALUES (1, '2022-10-10');
```

解析器看到 `'2022-10-10'` 时，并不会立刻把它当成 DATE 类型，而是先创建一个 CHARS 类型的 `Value`。

原因很简单：

```sql
INSERT INTO t(name) VALUES ('2022-10-10');
```

如果 `name` 是 CHAR 字段，这个字符串应该原样保存，不能被解析器强制改成日期。

所以真正的转换发生在“值类型和字段类型不一致”的时候。

### 6.2 `Value::cast_to` 调用源类型的转换函数

`Value::cast_to` 的实现是：

```cpp
DataType::type_instance(value.attr_type())->cast_to(value, to_type, result);
```

注意这里调用的是**源类型**的 `cast_to`。

所以：

- 字符串 → 日期，要改 `CharType::cast_to`；
- 日期 → 字符串，要改 `DateType::cast_to`。

这是理解类型转换时最容易混淆的一点。

### 6.3 `CharType::cast_to` 增加 DATES 分支

文件：`src/observer/common/type/char_type.cpp`

```cpp
case AttrType::DATES: {
  int date_value = 0;
  RC  rc         = DateType::parse_date(val.get_string(), date_value);
  if (OB_FAIL(rc)) {
    return rc;
  }
  result.set_date(date_value);
  return RC::SUCCESS;
}
```

`DateType::parse_date` 负责检查日期是否合法。非法时返回错误，调用方会把整条 SQL 判为失败。

### 6.4 `cast_cost` 让优化器愿意把字符串转成日期

只实现 `cast_to` 还不够。比较 `DATE` 字段和字符串时，优化器需要知道“哪一边转换成哪一边更便宜”。

在 `CharType::cast_cost` 中增加：

```cpp
if (type == AttrType::DATES) {
  return 1;
}
```

于是：

```text
DATE  -> CHARS : 代价 INT32_MAX（不推荐）
CHARS -> DATE  : 代价 1（推荐）
```

比较条件 `birthday = '2022-10-10'` 时，优化器会把右边的字符串转成 DATE，而不是把左边的 DATE 字段转成字符串。

### 6.5 INSERT 的转换路径

```text
INSERT INTO t VALUES (1, '2022-10-10');
  -> Table::make_record
  -> 字段类型是 DATES，值类型是 CHARS
  -> Value::cast_to(CHARS -> DATES)
  -> CharType::cast_to
  -> DateType::parse_date
  -> Value::set_date
  -> 4 字节写入记录
```

`Table::make_record` 中的关键逻辑是：

```cpp
if (field->type() != value.attr_type()) {
  Value real_value;
  rc = Value::cast_to(value, field->type(), real_value);
  ...
  rc = set_value_to_record(record_data, real_value, field);
} else {
  rc = set_value_to_record(record_data, value, field);
}
```

### 6.6 WHERE 的转换路径

```text
SELECT * FROM t WHERE birthday = '2022-10-10';
  -> LogicalPlanGenerator 发现 DATE 和 CHARS 类型不同
  -> 根据 cast_cost 选择把字符串转成 DATE
  -> 插入 CastExpr
  -> 如果右边是常量，直接调用 try_get_value 求值
  -> 得到 DATE 类型的 ValueExpr
  -> 比较时两边都是 DATE
```

常量在生成计划时就会完成转换。因此非法日期会在生成计划阶段直接失败，而不是等扫描到某一行才报错。

---

## 7. DateType 的解析、比较和格式化

### 7.1 解析：`parse_date`

文件：`src/observer/common/type/date_type.cpp`

解析规则：

- 允许字符串前后有空白；
- 中间必须是“数字 - 数字 - 数字”；
- 年 1~4 位数字；
- 月、日 1~2 位数字；
- 不能有尾随字符；
- 年 1~9999；
- 月 1~12；
- 日 1~当月最大天数；
- 闰年规则：能被 400 整除，或者能被 4 整除但不能被 100 整除。

核心代码：

```cpp
bool is_leap_year(int year)
{
  return (year % 400 == 0) || (year % 4 == 0 && year % 100 != 0);
}

int days_in_month(int year, int month)
{
  static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && is_leap_year(year)) {
    return 29;
  }
  return days[month];
}
```

因此：

| 日期 | 结果 | 原因 |
| --- | --- | --- |
| `2016-2-29` | 合法 | 2016 是闰年 |
| `2017-2-29` | 非法 | 2017 不是闰年 |
| `1900-2-29` | 非法 | 1900 能被 100 整除但不能被 400 整除 |
| `2000-2-29` | 合法 | 2000 能被 400 整除 |
| `2017-11-31` | 非法 | 11 月只有 30 天 |
| `2017-12-32` | 非法 | 12 月只有 31 天 |

### 7.2 比较：`compare`

因为保存的是 `YYYYMMDD` 整数，比较非常直接：

```cpp
const int left_value  = left.get_date();
const int right_value = right.get_date();
if (left_value < right_value) return -1;
if (left_value > right_value) return 1;
return 0;
```

这样 `>`、`<`、`=`、`>=`、`<=`、`<>` 都能复用已有的比较算子。

### 7.3 输出：`to_string`

```cpp
snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
         date_value / 10000,
         (date_value % 10000) / 100,
         date_value % 100);
```

例如：

```text
20200101 -> 2020-01-01
20221010 -> 2022-10-10
```

月份和日期不足两位时补 `0`，符合题目要求的 `YYYY-mm-dd` 输出。

### 7.4 从文件导入：`set_value_from_str`

`LOAD DATA` 走的是 `DataType::type_instance(field->type())->set_value_from_str(...)`。

`DateType` 实现了：

```cpp
int date_value = 0;
RC  rc         = parse_date(data, date_value);
if (OB_SUCC(rc)) {
  val.set_date(date_value);
}
return rc;
```

这样从文件导入 DATE 字段时也能做同样的校验。

---

## 8. 语法解析：让建表语句认识 DATE

文件：`src/observer/sql/parser/lex_sql.l`、`src/observer/sql/parser/yacc_sql.y`

### 8.1 词法分析

在关键字列表中加入：

```text
DATE    RETURN_TOKEN(DATE_T);
```

该文件开启了 `%option case-insensitive`，所以 `DATE`、`date`、`Date` 都能识别。

### 8.2 语法分析

增加 token：

```text
%token ... DATE_T ...
```

并在 `type` 规则中增加：

```text
type:
    INT_T      { $$ = static_cast<int>(AttrType::INTS); }
    | STRING_T { $$ = static_cast<int>(AttrType::CHARS); }
    | FLOAT_T  { $$ = static_cast<int>(AttrType::FLOATS); }
    | DATE_T   { $$ = static_cast<int>(AttrType::DATES); }
    | VECTOR_T { $$ = static_cast<int>(AttrType::VECTORS); }
    ;
```

`CREATE TABLE t(id INT, birthday DATE)` 时：

```text
birthday 的字段类型 = AttrType::DATES
字段长度 = 语法中的默认值 4
```

### 8.3 生成文件

`lex_sql.cpp`、`yacc_sql.cpp`、`yacc_sql.hpp` 是 CMake 调用 flex/bison 自动生成的，仓库的 `.gitignore` 已经忽略它们。

构建时会根据 `.l` 和 `.y` 重新生成，所以修改源码后重新执行 CMake 和 make 即可，不需要手动改生成文件。

---

## 9. 存储与索引：DATE 为什么能直接工作

### 9.1 记录中的 DATE

DATE 字段长度固定为 4 字节。插入时，`Table::make_record` 把 `Value::data()` 指向的 4 字节拷贝到记录中：

```text
record_data + birthday_offset  ->  20221010（int32）
```

读取时：

```cpp
cell.set_type(field_meta->type());          // DATES
cell.set_data(record + offset, 4);          // 读出 int32
```

### 9.2 B+ 树索引

建索引：

```sql
CREATE INDEX index_id ON date_table(u_date);
```

B+ 树的 `AttrComparator` 会把索引键的原始字节转成 `Value`：

```cpp
Value left;
left.set_type(attr_type_);       // DATES
left.set_data(v1, attr_length_);
Value right;
right.set_type(attr_type_);
right.set_data(v2, attr_length_);
return DataType::type_instance(attr_type_)->compare(left, right);
```

只要 `Value::set_data` 支持 DATES，日期索引键就能按整数正确比较。

等值查询：

```sql
SELECT * FROM date_table WHERE u_date = '2000-01-01';
```

计划会变成：

```text
PROJECT
└─INDEX_SCAN(index_id ON date_table)
```

### 9.3 元数据和重启

`FieldMeta::to_json` 保存：

```json
{
  "name": "u_date",
  "type": "dates",
  "len": 4
}
```

重启时 `attr_type_from_string("dates")` 还原成 `AttrType::DATES`，因此 DATE 表和 DATE 索引都能正常重新加载。

---

## 10. 非法日期为什么返回 FAILURE

非法日期有两个入口。

### 10.1 INSERT 非法日期

```sql
INSERT INTO t VALUES (10, '2017-2-29');
```

执行路径：

```text
Table::make_record
  -> Value::cast_to(CHARS -> DATES)
  -> CharType::cast_to
  -> DateType::parse_date
  -> 返回错误
  -> make_record 返回错误
  -> 客户端收到 FAILURE
```

### 10.2 WHERE 非法日期

```sql
SELECT * FROM t WHERE birthday = '2017-2-29';
```

执行路径：

```text
LogicalPlanGenerator
  -> 发现 DATE 和 CHARS 类型不同
  -> 插入 CastExpr
  -> CastExpr::try_get_value
  -> DateType::parse_date
  -> 返回错误
  -> 生成计划失败
  -> 客户端收到 FAILURE
```

所以非法日期不是“查不到数据”，而是整条 SQL 失败，符合题目要求。

### 10.3 已经验证的非法输入

```text
2017-2-29     -> FAILURE（不是闰年）
2017-21-29    -> FAILURE（月份越界）
2017-12-32    -> FAILURE（日期越界）
2017-11-31    -> FAILURE（11 月只有 30 天）
1900-2-29     -> FAILURE（100 年规则）
2100-2-29     -> FAILURE（100 年规则）
2000-2-30     -> FAILURE（2 月没有 30 日）
2020-00-10    -> FAILURE（月份为 0）
2020-13-10    -> FAILURE（月份为 13）
2020-01-00    -> FAILURE（日期为 0）
2020/01/01    -> FAILURE（分隔符不是 -）
2020-1-01x    -> FAILURE（有尾随字符）
空字符串       -> FAILURE
0000-1-1      -> FAILURE（年份为 0）
10000-1-1     -> FAILURE（年份超过 9999）
```

---

## 11. `<>` 查询没有结果：一个优化器 bug

这是测试中实际遇到的问题，值得单独说明。

### 11.1 现象

测试查询：

```sql
SELECT *
FROM date_table
WHERE u_date <> '2000-01-01'
  AND u_date < '2019-12-21';
```

表中 `u_date` 有索引：

```sql
CREATE INDEX index_id ON date_table(u_date);
```

期望结果包含：

```text
10 | 1950-02-02
6  | 2016-02-29
7  | 1970-01-01
```

实际结果为空。

### 11.2 根因

问题不在日期解析，而在物理计划生成器选择索引时，把 `<>` 也当成了“可以用等值索引”的条件。

原有代码：

```cpp
// 简单处理，就找等值查询
if (comparison_expr->comp() != EQUAL_TO && comparison_expr->comp() != NOT_EQUAL) {
  continue;
}
```

但后面的索引扫描构造是：

```cpp
IndexScanPhysicalOperator(table,
    index,
    mode,
    &value,
    true,   // left_inclusive
    &value,
    true);  // right_inclusive
```

左右边界都等于同一个值，这实际上是一个**等值范围扫描**，只适合 `=`。

对 `<>` 来说，它会先只扫描 `2000-01-01` 这一行，然后外层谓词 `u_date <> '2000-01-01'` 又把这行过滤掉，最终结果自然是空。

### 11.3 修复

只让 `EQUAL_TO` 使用这种等值索引扫描：

```cpp
// 简单处理，就找等值查询。
// 注意：NOT_EQUAL 不能复用下面的“左右边界都等于 value”的索引扫描，
// 否则会退化成只扫描 value 这一行，再被 <> 谓词过滤掉，导致结果为空。
if (comparison_expr->comp() != EQUAL_TO) {
  continue;
}
```

`<>` 改为走普通表扫描，由谓词逐行判断：

```text
修复前：
PROJECT
└─INDEX_SCAN(index_id ON date_table)
结果：0 行

修复后：
PROJECT
└─TABLE_SCAN(date_table)
结果：3 行
```

修复后等值查询仍然走索引：

```text
SELECT * FROM date_table WHERE u_date = '2000-01-01';

PROJECT
└─INDEX_SCAN(index_id ON date_table)
```

### 11.4 为什么这不是 DATE 类型本身的问题

`<>` 的索引选择 bug 对 INT、FLOAT、CHAR 同样存在，只是 DATE 测试恰好用了：

```sql
u_date <> '...' AND u_date < '...'
```

并且 DATE 表上建了索引，所以暴露出来。

这个修复是通用的：任何类型的 `<>` 都不应该走“左右边界都相等”的索引扫描。

---

## 12. 测试与验证

### 12.1 编译

在 Docker 容器中：

```bash
cd /root/miniob/build
cmake ..
make -j4
```

### 12.2 官方 primary-date 用例

```bash
cd /root/miniob/test/case
python3 miniob_test.py \
  --project-dir=/root/miniob \
  --work-dir=/root/miniob \
  --test-cases=primary-date \
  --report-only
```

关键结果：

```text
SELECT * FROM date_table WHERE u_date > '2020-1-20';
1 | 2020-01-21
2 | 2020-10-21
9 | 2038-01-19

SELECT * FROM date_table WHERE u_date < '2019-12-31';
5 | 2019-12-21
6 | 2016-02-29
7 | 1970-01-01
8 | 2000-01-01

SELECT * FROM date_table WHERE u_date = '2020-1-1';
3 | 2020-01-01
4 | 2020-01-01

SELECT * FROM date_table WHERE u_date = '2017-2-29';
FAILURE

INSERT INTO date_table VALUES (10, '2017-2-29');
FAILURE
```

### 12.3 边界用例

额外验证过：

- `2039-12-31`
- `1969-12-31`
- `2100-2-28`
- `9999-12-31`
- `1000-01-01`
- `0001-01-01`

都能正常插入、比较和输出。

### 12.4 索引与 `<>` 修复验证

```sql
SELECT * FROM date_table
WHERE u_date <> '2000-01-01'
  AND u_date < '2019-12-21';
```

修复后输出：

```text
6  | 2016-02-29
7  | 1970-01-01
10 | 1950-02-02
```

排序后就是：

```text
10 | 1950-02-02
6  | 2016-02-29
7  | 1970-01-01
```

### 12.5 重启验证

创建带 DATE 字段和 DATE 索引的表，停止 observer，再启动：

```sql
SELECT * FROM date_table;
SELECT * FROM date_table WHERE u_date = '2039-1-1';
```

结果正常，说明：

- 表元数据中的 `"dates"` 能正确加载；
- 索引文件中的 DATE 类型能正确恢复；
- DATE 索引键仍能正确比较。

---

## 13. 没有实现的部分

按题目要求，只实现 DATE 字段本身，没有额外扩展：

- 没有实现日期加减，例如 `birthday + 1`；
- 没有实现 `DATE_ADD`、`DATEDIFF` 等日期函数；
- 没有实现 DATE 的聚合函数；
- 没有专门适配向量化执行模式中的 DATE 比较；
- 这个仓库的 `UpdateStmt::create` 本身还是 TODO，UPDATE 整体未实现，因此没有单独实现 DATE 的 UPDATE。

如果以后补全 UPDATE，复用 `Value::cast_to` / `CharType::cast_to` 就能自动获得日期校验能力。

### 13.1 输入格式

支持：

```text
2020-01-01
2020-1-1
2020-01-1
2020-1-01
```

不支持：

```text
2020/01/01
2020-01-01x
2020-13-01
```

### 13.2 年份范围

当前限制为 1~9999：

- `0000-1-1` 失败；
- `10000-1-1` 失败；
- `0001-01-01` 合法；
- `9999-12-31` 合法。

### 13.3 DATE 是关键字

`DATE` 被加入词法关键字列表，因此和 `INT`、`CHAR` 一样，不建议把列名或表名直接命名为 `date`。

---

## 14. 关键函数索引

| 函数 | 文件 | 作用 |
| --- | --- | --- |
| `DateType::parse_date` | `src/observer/common/type/date_type.cpp` | 解析并校验日期 |
| `DateType::compare` | 同上 | 比较两个日期 |
| `DateType::to_string` | 同上 | 格式化为 `YYYY-MM-DD` |
| `DateType::cast_to` | 同上 | DATE 转 CHARS 等 |
| `DateType::set_value_from_str` | 同上 | 从文件导入日期 |
| `CharType::cast_to` | `src/observer/common/type/char_type.cpp` | 字符串转 DATE |
| `CharType::cast_cost` | 同上 | 让优化器选择 CHARS -> DATE |
| `Value::set_data` | `src/observer/common/value.cpp` | 从记录/索引键读取 DATE |
| `Value::set_date` / `get_date` | 同上 | VALUE 中存取日期 |
| `Value::get_int` / `get_float` / `get_boolean` | 同上 | 类型转换兼容 |
| `PhysicalPlanGenerator::create_plan` | `src/observer/sql/optimizer/physical_plan_generator.cpp` | 修复 `<>` 误用等值索引 |

---

## 15. 复现实验

在 Docker 容器中启动 observer：

```bash
cd /root/miniob
./build/bin/observer -f etc/observer.ini -s /tmp/miniob.sock
```

另开一个终端连接：

```bash
./build/bin/obclient -s /tmp/miniob.sock
```

执行：

```sql
CREATE TABLE date_table(id INT, u_date DATE);
CREATE INDEX index_id ON date_table(u_date);

INSERT INTO date_table VALUES (1, '2020-01-21');
INSERT INTO date_table VALUES (2, '2020-10-21');
INSERT INTO date_table VALUES (3, '2020-1-01');
INSERT INTO date_table VALUES (6, '2016-2-29');
INSERT INTO date_table VALUES (7, '1970-1-1');
INSERT INTO date_table VALUES (8, '2000-01-01');
INSERT INTO date_table VALUES (10, '1950-02-02');

SELECT * FROM date_table
WHERE u_date <> '2000-01-01'
  AND u_date < '2019-12-21';
```

结果应包含：

```text
10 | 1950-02-02
6  | 2016-02-29
7  | 1970-01-01
```

再执行：

```sql
EXPLAIN SELECT * FROM date_table
WHERE u_date <> '2000-01-01'
  AND u_date < '2019-12-21';
```

应看到：

```text
PROJECT
└─TABLE_SCAN(date_table)
```

而：

```sql
EXPLAIN SELECT * FROM date_table WHERE u_date = '2000-01-01';
```

应看到：

```text
PROJECT
└─INDEX_SCAN(index_id ON date_table)
```

---

## 16. 总结

这道题的关键不是“识别一个 DATE 关键字”，而是把一个新类型完整接入数据库已有的类型系统：

1. 解析层认识 DATE；
2. 类型系统注册 DATES 和 DateType；
3. Value 能携带日期；
4. 字符串能转换成日期，并做合法性校验；
5. 日期能按 4 字节写入记录；
6. 索引键能按日期比较；
7. 查询结果能格式化成 `YYYY-MM-DD`；
8. 非法日期在 INSERT 和 WHERE 中都能返回 FAILURE。

存储上选择 `YYYYMMDD` 整数，是为了同时满足“4 字节”“1970 年前可表示”“2038 年后可表示”“比较简单”这几个要求。

另外，测试中暴露的 `<>` 查询问题不是 DATE 解析错误，而是优化器把 `<>` 错误地当成等值索引查询。修复方式是只让 `EQUAL_TO` 使用等值索引扫描，`<>` 退回表扫描，由谓词正确过滤。
