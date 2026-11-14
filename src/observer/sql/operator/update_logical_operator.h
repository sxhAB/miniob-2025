/* Copyright (c) 2026 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2 */

#pragma once

#include "sql/operator/logical_operator.h"

class UpdateLogicalOperator : public LogicalOperator
{
public:
  UpdateLogicalOperator(Table *table, const FieldMeta *field, const Value &value)
      : table_(table), field_(field), value_(value)
  {}

  LogicalOperatorType type() const override { return LogicalOperatorType::UPDATE; }
  OpType get_op_type() const override { return OpType::LOGICALUPDATE; }

  Table *table() const { return table_; }
  const FieldMeta *field() const { return field_; }
  const Value &value() const { return value_; }

private:
  Table           *table_;
  const FieldMeta *field_;
  Value            value_;
};
