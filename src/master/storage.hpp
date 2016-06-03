// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STORAGE_HPP_
#define __STORAGE_HPP_

#include <string>

#include <mesos/state/protobuf.hpp>

#include <process/id.hpp>
#include <process/process.hpp>

using mesos::state::protobuf::State;
using mesos::state::protobuf::Variable;

using process::Process;

namespace mesos {
namespace internal {
namespace master {

class StorageProcess : public Process<StorageProcess>
{
public:
  StorageProcess(State* _state)
    : ProcessBase(process::ID::generate("storage")),
      state(_state){}

  process::Future<Nothing> create(const std::string& name);

  // Returns a variable from the state, creating a new one if one
  // previously did not exist (or an error if one occurs).
  template <typename T>
  process::Future<Variable<T>> fetch(
      const std::string& storage,
      const std::string& key);

  // Returns the variable specified if it was successfully stored in
  // the state, otherwise returns none if the version of the
  // variable
  // was no longer valid, or an error if one occurs.
  template <typename T>
  process::Future<Option<Variable<T>>> store(
      const std::string& storage,
      const Variable<T>& variable);

  // Expunges the variable from the state.
  template <typename T>
  process::Future<bool> expunge(
      const std::string& storage,
      const Variable<T>& variable);

private:
  State* state;
};

} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __STORAGE_HPP_
