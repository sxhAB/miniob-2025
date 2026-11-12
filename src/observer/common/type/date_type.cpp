/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "common/type/date_type.h"

#include <ctype.h>
#include <stdio.h>

#include "common/log/log.h"
#include "common/value.h"

namespace {

bool is_leap_year(int year) { return (year % 400 == 0) || (year % 4 == 0 && year % 100 != 0); }

int days_in_month(int year, int month)
{
  static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && is_leap_year(year)) {
    return 29;
  }
  return days[month];
}

/**
 * @brief 从 pos 开始读取一个不超过 max_digits 位的十进制整数
 * @details 读取完后 pos 指向数字之后的第一个字符。至少需要有一位数字。
 */
bool read_number(const string &str, size_t &pos, int max_digits, int &value)
{
  const size_t start  = pos;
  int          result = 0;
  while (pos < str.size() && isdigit(static_cast<unsigned char>(str[pos]))) {
    if (static_cast<int>(pos - start) >= max_digits) {
      return false;
    }
    result = result * 10 + (str[pos] - '0');
    ++pos;
  }
  if (pos == start) {
    return false;
  }
  value = result;
  return true;
}

}  // namespace

RC DateType::parse_date(const string &input, int &date_value)
{
  // 允许字符串前后有空白（比如从文件中导入数据时），中间不允许有空白
  size_t begin = 0;
  size_t end   = input.size();
  while (begin < end && isspace(static_cast<unsigned char>(input[begin]))) {
    ++begin;
  }
  while (end > begin && isspace(static_cast<unsigned char>(input[end - 1]))) {
    --end;
  }

  if (begin == end) {
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }

  string str = input.substr(begin, end - begin);

  size_t pos   = 0;
  int    year  = 0;
  int    month = 0;
  int    day   = 0;

  // 年：1~4 位数字
  if (!read_number(str, pos, 4, year) || pos >= str.size() || str[pos] != '-') {
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }
  ++pos;

  // 月：1~2 位数字
  if (!read_number(str, pos, 2, month) || pos >= str.size() || str[pos] != '-') {
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }
  ++pos;

  // 日：1~2 位数字
  if (!read_number(str, pos, 2, day) || pos != str.size()) {
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }

  // 检查日期是否合法。年份限制在 1~9999，保证可以按 %04d-%02d-%02d 输出。
  if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1 || day > days_in_month(year, month)) {
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }

  date_value = year * 10000 + month * 100 + day;
  return RC::SUCCESS;
}

int DateType::compare(const Value &left, const Value &right) const
{
  ASSERT(left.attr_type() == AttrType::DATES, "left type is not date");
  if (right.attr_type() != AttrType::DATES) {
    return INT32_MAX;
  }

  const int left_value  = left.get_date();
  const int right_value = right.get_date();
  if (left_value < right_value) {
    return -1;
  }
  if (left_value > right_value) {
    return 1;
  }
  return 0;
}

RC DateType::cast_to(const Value &val, AttrType type, Value &result) const
{
  switch (type) {
    case AttrType::DATES: {
      result.set_date(val.get_date());
      return RC::SUCCESS;
    }
    case AttrType::CHARS: {
      string str;
      RC     rc = to_string(val, str);
      if (OB_FAIL(rc)) {
        return rc;
      }
      result.set_string(str.c_str());
      return RC::SUCCESS;
    }
    default: {
      LOG_WARN("unsupported cast from DATE to %s", attr_type_to_string(type));
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
  }
}

RC DateType::set_value_from_str(Value &val, const string &data) const
{
  int date_value = 0;
  RC  rc         = parse_date(data, date_value);
  if (OB_SUCC(rc)) {
    val.set_date(date_value);
  }
  return rc;
}

RC DateType::to_string(const Value &val, string &result) const
{
  const int date_value = val.get_date();
  char      buf[16]    = {0};
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", date_value / 10000, (date_value % 10000) / 100, date_value % 100);
  result = buf;
  return RC::SUCCESS;
}
