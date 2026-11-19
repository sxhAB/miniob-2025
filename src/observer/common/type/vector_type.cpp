#include "common/type/vector_type.h"
#include "common/value.h"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <vector>

RC VectorType::set_value_from_str(Value &val, const string &data) const
{
  std::istringstream input(data);
  input.imbue(std::locale::classic());
  char delimiter;
  if (!(input >> delimiter) || delimiter != '[') {
    return RC::INVALID_ARGUMENT;
  }
  std::vector<float> elements;
  do {
    float element;
    if (!(input >> element) || !std::isfinite(element) || elements.size() >= MAX_DIMENSION) {
      return RC::INVALID_ARGUMENT;
    }
    elements.push_back(element);
    if (!(input >> delimiter)) {
      return RC::INVALID_ARGUMENT;
    }
  } while (delimiter == ',');

  if (delimiter != ']' || !(input >> std::ws).eof()) {
    return RC::INVALID_ARGUMENT;
  }
  val.set_vector(elements.data(), elements.size());
  return RC::SUCCESS;
}

RC VectorType::to_string(const Value &val, string &result) const
{
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << '[' << std::scientific << std::setprecision(5);
  for (int i = 0; i < val.vector_dimension(); ++i) {
    if (i != 0) {
      output << ',';
    }
    output << val.vector_at(i);
  }
  output << ']';
  result = output.str();
  return RC::SUCCESS;
}

int VectorType::compare(const Value &left, const Value &right) const
{
  if (right.attr_type() != AttrType::VECTORS) {
    return INT32_MAX;
  }
  if (left.vector_dimension() != right.vector_dimension()) {
    return 1;
  }
  for (int i = 0; i < left.vector_dimension(); ++i) {
    if (left.vector_at(i) != right.vector_at(i)) {
      return 1;
    }
  }
  return 0;
}
