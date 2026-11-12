/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "common/type/data_type.h"

/**
 * @brief 日期类型
 * @ingroup DataType
 * @details DATE 类型在内存中固定占用 4 个字节，保存成 YYYYMMDD 形式的整数。
 * 例如 2022-10-10 保存为 20221010。
 *
 * 这样保存有两个好处：
 * 1. 只需要 4 个字节，和 INT 一样，可以直接放进记录里；
 * 2. YYYYMMDD 的整数大小顺序与日期的先后顺序一致，比较时直接比较整数即可，
 *    并且不受 2038 年时间戳溢出、也不受 1970 年之前无法表示的限制。
 */
class DateType : public DataType
{
public:
  DateType() : DataType(AttrType::DATES) {}
  ~DateType() override = default;

  int compare(const Value &left, const Value &right) const override;

  RC cast_to(const Value &val, AttrType type, Value &result) const override;

  RC set_value_from_str(Value &val, const string &data) const override;

  RC to_string(const Value &val, string &result) const override;

  /**
   * @brief 把 `YYYY-MM-DD` 格式的字符串解析成 YYYYMMDD 整数
   * @param str 待解析的字符串，例如 "2022-10-10" 或 "2022-1-1"
   * @param[out] date_value 解析成功时写入 YYYYMMDD 形式的整数
   * @return 成功返回 RC::SUCCESS；格式或日期非法时返回 RC::SCHEMA_FIELD_TYPE_MISMATCH
   */
  static RC parse_date(const string &str, int &date_value);
};
