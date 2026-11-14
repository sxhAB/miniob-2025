/* Copyright (c) 2026 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2 */

#pragma once

#include "sql/operator/physical_operator.h"

class UpdatePhysicalOperator : public PhysicalOperator
{
public:
  UpdatePhysicalOperator(Table *table, const FieldMeta *field, const Value &value)
      : table_(table), field_(field), value_(value)
  {}

  PhysicalOperatorType type() const override { return PhysicalOperatorType::UPDATE; }
  OpType get_op_type() const override { return OpType::UPDATE; }

  RC open(Trx *trx) override;
  RC next() override { return RC::RECORD_EOF; }
  RC close() override { return result_; }

private:
  RC execute(Trx *trx);

  Table           *table_;
  const FieldMeta *field_;
  Value            value_;
  RC               result_ = RC::SUCCESS;
};
