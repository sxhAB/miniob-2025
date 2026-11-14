/* Copyright (c) 2026 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2 */

#include "sql/operator/update_physical_operator.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"

RC UpdatePhysicalOperator::open(Trx *trx)
{
  result_ = execute(trx);
  return result_;
}

RC UpdatePhysicalOperator::execute(Trx *trx)
{
  if (trx == nullptr || children_.size() != 1) {
    return RC::INTERNAL;
  }

  PhysicalOperator &child = *children_[0];
  RC rc = child.open(trx);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  // Own copies: scanner records may refer to pages released by next()/close().
  // Finish scanning before changing records or index entries.
  vector<Record> records;
  while ((rc = child.next()) == RC::SUCCESS) {
    Tuple *tuple = child.current_tuple();
    if (tuple == nullptr) {
      rc = RC::INTERNAL;
      break;
    }
    const Record &record = static_cast<RowTuple *>(tuple)->record();
    Record copy;
    rc = copy.copy_data(record.data(), record.len());
    if (rc != RC::SUCCESS) {
      break;
    }
    copy.set_rid(record.rid());
    records.emplace_back(std::move(copy));
  }

  RC close_rc = child.close();
  if (rc != RC::RECORD_EOF) {
    return rc;
  }
  if (close_rc != RC::SUCCESS) {
    return close_rc;
  }

  for (Record &old_record : records) {
    Record new_record;
    rc = table_->make_update_record(old_record, *field_, value_, new_record);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    rc = trx->update_record(table_, old_record, new_record);
    if (rc != RC::SUCCESS) {
      return rc;
    }
  }
  return RC::SUCCESS;
}
