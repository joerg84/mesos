/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>

#include <algorithm>
#include <iostream>
#include <set>
#include <vector>

#include <glog/logging.h>

#include <boost/lexical_cast.hpp>

#include <mesos/resources.hpp>
#include <mesos/values.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/strings.hpp>

using std::max;
using std::min;
using std::ostream;
using std::string;
using std::vector;

namespace mesos {

ostream& operator<< (ostream& stream, const Value::Scalar& scalar)
{
  return stream << scalar.value();
}


bool operator==(const Value::Scalar& left, const Value::Scalar& right)
{
  return left.value() == right.value();
}


bool operator<=(const Value::Scalar& left, const Value::Scalar& right)
{
  return left.value() <= right.value();
}


Value::Scalar operator+(const Value::Scalar& left, const Value::Scalar& right)
{
  Value::Scalar result;
  result.set_value(left.value() + right.value());
  return result;
}


Value::Scalar operator-(const Value::Scalar& left, const Value::Scalar& right)
{
  Value::Scalar result;
  result.set_value(left.value() - right.value());
  return result;
}


Value::Scalar& operator+=(Value::Scalar& left, const Value::Scalar& right)
{
  left.set_value(left.value() + right.value());
  return left;
}


Value::Scalar& operator-=(Value::Scalar& left, const Value::Scalar& right)
{
  left.set_value(left.value() - right.value());
  return left;
}


namespace internal
{

struct Range
{
  uint64_t start;
  uint64_t end;
};


// Coalesces the `ranges` provided and modified `result` to contain the
// solution.
// The algorithm first sorts all the individual intervals so that we can iterate
// over them sequentially.
// The algorithm does a single pass, after the sort, and builds up the solution
// in place. It then modified the `result` with as few steps as possible. The
// expensive part of this operation is modification of the protbuf, which is why
// we prefer to build up the solution in a temporary vector.
void coalesce(Value::Ranges* result, std::vector<Range>&& ranges) {
  // Exit early if empty.
  if (ranges.empty()) {
    result->clear_range();
    return;
  }

  std::sort(
      ranges.begin(),
      ranges.end(),
      [](const Range& lhs, const Range& rhs) {
        return lhs.start < rhs.start ||
          (lhs.start == rhs.start && lhs.end < rhs.end);
      });

  // We build up initial state of the current range.
  CHECK(!ranges.empty());
  size_t count = 1;
  uint64_t currentStart = ranges.front().start;
  uint64_t currentEnd = ranges.front().end;

  // In a single pass, we compute the size of the end result, as well as modify
  // in place the intermediate data structure to build up the soltion as we
  // solve it.
  foreach (const Range& range, ranges) {
    // Skip if this range is equivalent to the current range.
    if (range.start == currentStart && range.end == currentEnd) {
      continue;
    }

    // If the current range just needs to be extended on the right.
    if (range.start == currentStart && range.end > currentEnd) {
      currentEnd = range.end;
    }

    // If we are starting farther ahead, then there are 2 cases.
    else if (range.start > currentStart) {
      // The range is contiguous.
      if (range.start <= currentEnd + 1) {
        currentEnd = std::max(currentEnd, range.end);
      }

      // The previous range ended, and we are starting a new one.
      else {
        ranges[count - 1].start = currentStart;
        ranges[count - 1].end = currentEnd;
        ++count;
        currentStart = range.start;
        currentEnd = range.end;
      }
    }
  }

  // Record the state of the last range into the solution.
  ranges[count - 1].start = currentStart;
  ranges[count - 1].end = currentEnd;

  CHECK(count <= ranges.size());

  // Make sure we shrink if we're too big.
  if (count < result->range_size()) {
    result->mutable_range()
      ->DeleteSubrange(count, result->range_size() - count);
  }

  // Resize enough space so we allocate the pointer array just once.
  result->mutable_range()->Reserve(count);

  // Copy the solution from the ranges into result.
  for (size_t i = 0; i < count; ++i) {
    if (i >= result->range_size()) {
      result->add_range();
    }

    CHECK(i < result->range_size());
    result->mutable_range(i)->set_begin(ranges[i].start);
    result->mutable_range(i)->set_end(ranges[i].end);
  }

  CHECK_EQ(result->range_size(), count);
}

} // namespace internal {


// Coalesce the given 'rhs' ranges into 'lhs' ranges.
void coalesce(Value::Ranges* lhs, std::initializer_list<Value::Ranges> rhs) {
  size_t rhsSum = 0;
  foreach (const Value::Ranges& range, rhs) {
    rhsSum += range.range_size();
  }

  std::vector<internal::Range> ranges(lhs->range_size() + rhsSum);

  // Merges the ranges into a vector.
  auto fill = [&ranges](const Value::Ranges& inputs, size_t offset) {
    for (size_t i = 0; i < inputs.range_size(); ++i) {
      ranges[offset + i].start = inputs.range(i).begin();
      ranges[offset + i].end = inputs.range(i).end();
    }
  };

  // Fill the `lhs`.
  fill(*lhs, 0);

  // Fill each of the ranges in the `rhs`.
  size_t offset = lhs->range_size();
  foreach (const Value::Ranges& range, rhs) {
    fill(range, offset);
    offset += range.range_size();
  }

  internal::coalesce(lhs, std::move(ranges));
}


// Coalesce the given 'ranges'.
void coalesce(Value::Ranges* ranges) {
  coalesce(ranges, {Value::Ranges()});
}


// Coalesce the given '_rhs' into 'lhs' ranges.
void coalesce(Value::Ranges* lhs, const Value::Range& _rhs) {
  Value::Ranges rhs;
  rhs.add_range();
  rhs.mutable_range(0)->CopyFrom(_rhs);
  coalesce(lhs, {rhs});
}


// Removes a range from already coalesced ranges.
// Note that we assume that ranges has already been coalesced.
static void remove(Value::Ranges* _ranges, const Value::Range& removal)
{
  std::vector<internal::Range> ranges;
  ranges.reserve(_ranges->range_size());

  for (const Value::Range& range : _ranges->range()) {
    // Skip if the entire range is subsumed by `removal`.
    if (range.begin() >= removal.begin() && range.end() <= removal.end()) {
      continue;
    }

    // Divide if the range subsumes the `removal`.
    if (range.begin() < removal.begin() && range.end() > removal.end()) {
      // Front.
      ranges.emplace_back(internal::Range{range.begin(), removal.begin() - 1});
      // Back.
      ranges.emplace_back(internal::Range{removal.end() + 1, range.end()});
    }

    // Fully Emplace if the range doesn't intersect.
    if (range.end() < removal.begin() ||
        range.begin() > removal.end()) {
      ranges.emplace_back(internal::Range{range.begin(), range.end()});
    } else {
      // Trim if the range does intersect.
      if (range.end() > removal.end()) {
        // Trim front.
        ranges.emplace_back(internal::Range{removal.end() + 1, range.end()});
      } else {
        // Trim back.
        CHECK(range.begin() < removal.begin());
        ranges.emplace_back(
            internal::Range{range.begin(), removal.begin() - 1});
      }
    }
  }

  internal::coalesce(_ranges, std::move(ranges));
}


ostream& operator<<(ostream& stream, const Value::Ranges& ranges)
{
  stream << "[";
  for (int i = 0; i < ranges.range_size(); i++) {
    stream << ranges.range(i).begin() << "-" << ranges.range(i).end();
    if (i + 1 < ranges.range_size()) {
      stream << ", ";
    }
  }
  stream << "]";
  return stream;
}


bool operator==(const Value::Ranges& _left, const Value::Ranges& _right)
{
  Value::Ranges left;
  coalesce(&left, {_left});

  Value::Ranges right;
  coalesce(&right, {_right});

  if (left.range_size() == right.range_size()) {
    for (int i = 0; i < left.range_size(); i++) {
      // Make sure this range is equal to a range in the right.
      bool found = false;
      for (int j = 0; j < right.range_size(); j++) {
        if (left.range(i).begin() == right.range(j).begin() &&
            left.range(i).end() == right.range(j).end()) {
          found = true;
          break;
        }
      }

      if (!found) {
        return false;
      }
    }

    return true;
  }

  return false;
}


bool operator<=(const Value::Ranges& _left, const Value::Ranges& _right)
{
  Value::Ranges left;
  coalesce(&left, {_left});

  Value::Ranges right;
  coalesce(&right, {_right});

  for (int i = 0; i < left.range_size(); i++) {
    // Make sure this range is a subset of a range in right.
    bool matched = false;
    for (int j = 0; j < right.range_size(); j++) {
      if (left.range(i).begin() >= right.range(j).begin() &&
          left.range(i).end() <= right.range(j).end()) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      return false;
    }
  }

  return true;
}


Value::Ranges operator+(const Value::Ranges& left, const Value::Ranges& right)
{
  Value::Ranges result;
  coalesce(&result, {left, right});
  return result;
}


Value::Ranges operator-(const Value::Ranges& left, const Value::Ranges& right)
{
  Value::Ranges result;
  coalesce(&result, {left});
  return result -= right;
}


Value::Ranges& operator+=(Value::Ranges& left, const Value::Ranges& right)
{
  coalesce(&left, {right});
  return left;
}


Value::Ranges& operator-=(Value::Ranges& left, const Value::Ranges& right)
{
  coalesce(&left);
  for (int i = 0; i < right.range_size(); ++i) {
    remove(&left, right.range(i));
  }
  return left;
}


ostream& operator<<(ostream& stream, const Value::Set& set)
{
  stream << "{";
  for (int i = 0; i < set.item_size(); i++) {
    stream << set.item(i);
    if (i + 1 < set.item_size()) {
      stream << ", ";
    }
  }
  stream << "}";
  return stream;
}


bool operator==(const Value::Set& left, const Value::Set& right)
{
  if (left.item_size() == right.item_size()) {
    for (int i = 0; i < left.item_size(); i++) {
      // Make sure this item is equal to an item in the right.
      bool found = false;
      for (int j = 0; j < right.item_size(); j++) {
        if (left.item(i) == right.item(i)) {
          found = true;
          break;
        }
      }

      if (!found) {
        return false;
      }
    }

    return true;
  }

  return false;
}


bool operator<=(const Value::Set& left, const Value::Set& right)
{
  if (left.item_size() <= right.item_size()) {
    for (int i = 0; i < left.item_size(); i++) {
      // Make sure this item is equal to an item in the right.
      bool found = false;
      for (int j = 0; j < right.item_size(); j++) {
        if (left.item(i) == right.item(j)) {
          found = true;
          break;
        }
      }

      if (!found) {
        return false;
      }
    }

    return true;
  }

  return false;
}


Value::Set operator+(const Value::Set& left, const Value::Set& right)
{
  Value::Set result;

  for (int i = 0; i < left.item_size(); i++) {
    result.add_item(left.item(i));
  }

  // A little bit of extra logic to avoid adding duplicates from right.
  for (int i = 0; i < right.item_size(); i++) {
    bool found = false;
    for (int j = 0; j < result.item_size(); j++) {
      if (right.item(i) == result.item(j)) {
        found = true;
        break;
      }
    }

    if (!found) {
      result.add_item(right.item(i));
    }
  }

  return result;
}


Value::Set operator-(const Value::Set& left, const Value::Set& right)
{
  Value::Set result;

  // Look for the same item in right as we add left to result.
  for (int i = 0; i < left.item_size(); i++) {
    bool found = false;
    for (int j = 0; j < right.item_size(); j++) {
      if (left.item(i) == right.item(j)) {
        found = true;
        break;
      }
    }

    if (!found) {
      result.add_item(left.item(i));
    }
  }

  return result;
}


Value::Set& operator+=(Value::Set& left, const Value::Set& right)
{
  // A little bit of extra logic to avoid adding duplicates from right.
  for (int i = 0; i < right.item_size(); i++) {
    bool found = false;
    for (int j = 0; j < left.item_size(); j++) {
      if (right.item(i) == left.item(j)) {
        found = true;
        break;
      }
    }

    if (!found) {
      left.add_item(right.item(i));
    }
  }

  return left;
}


Value::Set& operator-=(Value::Set& left, const Value::Set& right)
{
  // For each item in right, remove it if it's in left.
  for (int i = 0; i < right.item_size(); i++) {
    for (int j = 0; j < left.item_size(); j++) {
      if (right.item(i) == left.item(j)) {
        left.mutable_item()->DeleteSubrange(j, 1);
        break;
      }
    }
  }

  return left;
}


ostream& operator<<(ostream& stream, const Value::Text& value)
{
  return stream << value.value();
}


bool operator==(const Value::Text& left, const Value::Text& right)
{
  return left.value() == right.value();
}


namespace internal {
namespace values {

Try<Value> parse(const std::string& text)
{
  Value value;

  // Remove any spaces from the text.
  string temp;
  foreach (const char c, text) {
    if (c != ' ') {
      temp += c;
    }
  }

  if (temp.length() == 0) {
    return Error("Expecting non-empty string");
  }

  // TODO(ynie): Find a better way to check brackets.
  if (!strings::checkBracketsMatching(temp, '{', '}') ||
      !strings::checkBracketsMatching(temp, '[', ']') ||
      !strings::checkBracketsMatching(temp, '(', ')')) {
    return Error("Mismatched brackets");
  }

  size_t index = temp.find('[');
  if (index == 0) {
    // This is a Value::Ranges.
    value.set_type(Value::RANGES);
    Value::Ranges* ranges = value.mutable_ranges();
    const vector<string>& tokens = strings::tokenize(temp, "[]-,\n");
    if (tokens.size() % 2 != 0) {
      return Error("Expecting one or more \"ranges\"");
    } else {
      for (size_t i = 0; i < tokens.size(); i += 2) {
        Value::Range* range = ranges->add_range();

        int j = i;
        try {
          range->set_begin(boost::lexical_cast<uint64_t>((tokens[j++])));
          range->set_end(boost::lexical_cast<uint64_t>(tokens[j++]));
        } catch (const boost::bad_lexical_cast&) {
          return Error(
              "Expecting non-negative integers in '" + tokens[j - 1] + "'");
        }
      }

      coalesce(ranges);

      return value;
    }
  } else if (index == string::npos) {
    size_t index = temp.find('{');
    if (index == 0) {
      // This is a set.
      value.set_type(Value::SET);
      Value::Set* set = value.mutable_set();
      const vector<string>& tokens = strings::tokenize(temp, "{},\n");
      for (size_t i = 0; i < tokens.size(); i++) {
        set->add_item(tokens[i]);
      }
      return value;
    } else if (index == string::npos) {
      try {
        // This is a scalar.
        value.set_type(Value::SCALAR);
        Value::Scalar* scalar = value.mutable_scalar();
        scalar->set_value(boost::lexical_cast<double>(temp));
        return value;
      } catch (const boost::bad_lexical_cast&) {
        // This is a text.
        value.set_type(Value::TEXT);
        Value::Text* text = value.mutable_text();
        text->set_value(temp);
        return value;
      }
    } else {
      return Error("Unexpected '{' found");
    }
  }

  return Error("Unexpected '[' found");
}


void sort(Value::Ranges* ranges)
{
  // Note that Range.begin() returns the first element of that range.
  std::sort(
      ranges->mutable_range()->begin(),
      ranges->mutable_range()->end(),
      [](const Value::Range& a, const Value::Range& b) {
        return a.begin() < b.begin();
      });
}

} // namespace values {
} // namespace internal {

} // namespace mesos {
