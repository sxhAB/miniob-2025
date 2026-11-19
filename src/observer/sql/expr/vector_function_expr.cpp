#include "sql/expr/expression.h"
#include "common/type/vector_type.h"

#include <algorithm>
#include <cmath>
#include <limits>

unique_ptr<Expression> VectorFunctionExpr::copy() const
{
  vector<unique_ptr<Expression>> arguments;
  for (const auto &argument : arguments_) {
    arguments.emplace_back(argument->copy());
  }
  auto result = make_unique<VectorFunctionExpr>(function_, std::move(arguments));
  result->set_name(name());
  return result;
}

bool VectorFunctionExpr::equal(const Expression &other) const
{
  if (other.type() != type()) {
    return false;
  }
  const auto &function = static_cast<const VectorFunctionExpr &>(other);
  if (function_ != function.function_ || arguments_.size() != function.arguments_.size()) {
    return false;
  }
  for (size_t i = 0; i < arguments_.size(); ++i) {
    if (!arguments_[i]->equal(*function.arguments_[i])) {
      return false;
    }
  }
  return true;
}

AttrType VectorFunctionExpr::value_type() const
{
  switch (function_) {
    case Function::FROM_STRING: return AttrType::VECTORS;
    case Function::TO_STRING: return AttrType::CHARS;
    case Function::DISTANCE: return AttrType::FLOATS;
  }
  return AttrType::UNDEFINED;
}

RC VectorFunctionExpr::validate() const
{
  const size_t expected = function_ == Function::DISTANCE ? 3 : 1;
  if (arguments_.size() != expected) {
    return RC::INVALID_ARGUMENT;
  }
  const AttrType first = function_ == Function::FROM_STRING ? AttrType::CHARS : AttrType::VECTORS;
  if (arguments_[0]->value_type() != first ||
      (function_ == Function::DISTANCE &&
          (arguments_[1]->value_type() != AttrType::VECTORS || arguments_[2]->value_type() != AttrType::CHARS))) {
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }
  return RC::SUCCESS;
}

RC VectorFunctionExpr::get_value(const Tuple &tuple, Value &value) const { return evaluate(&tuple, value); }
RC VectorFunctionExpr::try_get_value(Value &value) const { return evaluate(nullptr, value); }

RC VectorFunctionExpr::evaluate(const Tuple *tuple, Value &value) const
{
  RC rc = validate();
  if (rc != RC::SUCCESS) {
    return rc;
  }
  vector<Value> values(arguments_.size());
  for (size_t i = 0; i < arguments_.size(); ++i) {
    rc = tuple ? arguments_[i]->get_value(*tuple, values[i]) : arguments_[i]->try_get_value(values[i]);
    if (rc != RC::SUCCESS) {
      return rc;
    }
  }
  if (function_ == Function::FROM_STRING) {
    return VectorType().set_value_from_str(value, values[0].get_string());
  }
  if (function_ == Function::TO_STRING) {
    value.set_string(values[0].to_string().c_str());
    return RC::SUCCESS;
  }

  const Value &left = values[0];
  const Value &right = values[1];
  if (left.vector_dimension() == 0 || left.vector_dimension() != right.vector_dimension()) {
    return RC::INVALID_ARGUMENT;
  }
  double dot = 0, squared_distance = 0, left_norm = 0, right_norm = 0;
  for (int i = 0; i < left.vector_dimension(); ++i) {
    double a = left.vector_at(i), b = right.vector_at(i);
    dot += a * b;
    squared_distance += (a - b) * (a - b);
    left_norm += a * a;
    right_norm += b * b;
  }
  const string method = values[2].get_string();
  double distance;
  if (strcasecmp(method.c_str(), "EUCLIDEAN") == 0) {
    distance = std::sqrt(squared_distance);
  } else if (strcasecmp(method.c_str(), "DOT") == 0) {
    distance = dot;
  } else if (strcasecmp(method.c_str(), "COSINE") == 0) {
    if (left_norm == 0 || right_norm == 0) {
      return RC::INVALID_ARGUMENT;
    }
    const double cosine = dot / (std::sqrt(left_norm) * std::sqrt(right_norm));
    distance = 1.0 - std::clamp(cosine, -1.0, 1.0);
  } else {
    return RC::INVALID_ARGUMENT;
  }
  if (!std::isfinite(distance) || std::abs(distance) > std::numeric_limits<float>::max()) {
    return RC::INVALID_ARGUMENT;
  }
  value.set_float(static_cast<float>(distance));
  return RC::SUCCESS;
}
