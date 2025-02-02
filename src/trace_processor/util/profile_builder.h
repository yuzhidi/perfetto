/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_TRACE_PROCESSOR_UTIL_PROFILE_BUILDER_H_
#define SRC_TRACE_PROCESSOR_UTIL_PROFILE_BUILDER_H_

#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/packed_repeated_fields.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/third_party/pprof/profile.pbzero.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

// Builds a |perftools.profiles.Profile| proto.
class GProfileBuilder {
 public:
  // |sample_types| A description of the values stored with each sample.
  GProfileBuilder(
      TraceProcessorContext* context,
      const std::vector<std::pair<std::string, std::string>>& sample_types);
  ~GProfileBuilder();
  void AddSample(uint32_t callsite_id, const protozero::PackedVarInt& values);

  // Finalizes the profile and returns the serialized proto. Can be called
  // multiple times but after the first invocation `AddSample` calls will have
  // no effect.
  std::string Build();

 private:
  static constexpr int64_t kEmptyStringIndex = 0;

  // Strings are stored in the `Profile` in a table and referenced by their
  // index. This helper class takes care of all the book keeping.
  // `TraceProcessor` uses its own `StringPool` for strings. This helper
  // provides convenient ways of dealing with `StringPool::Id` values instead of
  // actual string. This class ensures that two equal strings will have the same
  // index, so you can compare them instead of the actual strings.
  class StringTable {
   public:
    // |result| This is the `Profile` proto we are building. Strings will be
    // added to it as necessary. |string_pool| `StringPool` to quey for strings
    // passed as `StringPool:Id`
    StringTable(protozero::HeapBuffered<
                    third_party::perftools::profiles::pbzero::Profile>* result,
                const StringPool* string_pool);

    // Adds the given string to the table, if not currently present, and returns
    // the index to it. Might write data to the infligt `Profile` so it should
    // not be called while in the middle of writing a message to the proto.
    int64_t InternString(base::StringView str);
    // Adds a string stored in the `TraceProcessor` `StringPool` to the table,
    // if not currently present, and returns the index to it. Might write data
    // to the inflight `Profile` so it should not be called while in the middle
    // of writing a message to the proto.
    int64_t InternString(StringPool::Id id);

   private:
    // Unconditionally writes the given string to the table and returns its
    // index.
    int64_t WriteString(base::StringView str);

    const StringPool& string_pool_;
    protozero::HeapBuffered<third_party::perftools::profiles::pbzero::Profile>&
        result_;

    std::unordered_map<StringPool::Id, int64_t> seen_string_pool_ids_;
    // Maps strings (hashes thereof) to indexes in the table.
    std::unordered_map<uint64_t, int64_t> seen_strings_;
    // Index where the next string will be written to
    int64_t next_index_{0};
  };

  struct Line {
    uint64_t function_id;
    int64_t line;
    bool operator==(const Line& other) const {
      return function_id == other.function_id && line == other.line;
    }
  };

  // Location, MappingKey, Mapping, Function, and Line are helper structs to
  // deduplicate entities. We do not write these directly to the proto Profile
  // but instead stage them and write them out during `Finalize`. Samples on the
  // other hand are directly written to the proto.

  struct Location {
    struct Hash {
      size_t operator()(const Location& loc) const {
        perfetto::base::Hash hasher;
        hasher.UpdateAll(loc.mapping_id, loc.rel_pc, loc.lines.size());
        for (const auto& line : loc.lines) {
          hasher.UpdateAll(line.function_id, line.line);
        }
        return static_cast<size_t>(hasher.digest());
      }
    };

    uint64_t mapping_id;
    uint64_t rel_pc;
    std::vector<Line> lines;

    bool operator==(const Location& other) const {
      return mapping_id == other.mapping_id && rel_pc == other.rel_pc &&
             lines == other.lines;
    }
  };

  // Mappings are tricky. We could have samples for different processes and
  // given address space layout randomization the same mapping could be located
  // at different addresses. MappingKey has the set of properties that uniquely
  // identify mapping in order to deduplicate rows in the stack_profile_mapping
  // table.
  struct MappingKey {
    struct Hash {
      size_t operator()(const MappingKey& mapping) const {
        perfetto::base::Hash hasher;
        hasher.UpdateAll(mapping.size, mapping.file_offset,
                         mapping.build_id_or_filename);
        return static_cast<size_t>(hasher.digest());
      }
    };

    explicit MappingKey(
        const tables::StackProfileMappingTable::ConstRowReference& mapping,
        StringTable& string_table);

    bool operator==(const MappingKey& other) const {
      return size == other.size && file_offset == other.file_offset &&
             build_id_or_filename == other.build_id_or_filename;
    }

    uint64_t size;
    uint64_t file_offset;
    int64_t build_id_or_filename;
  };

  // Keeps track of what debug information is available for a mapping.
  // TODO(carlscab): We could be a bit more "clever" here. Currently if there is
  // debug info for at least one frame we flag the mapping as having debug info.
  // We could use some heuristic instead, e.g. if x% for frames have the info
  // etc.
  struct DebugInfo {
    bool has_functions{false};
    bool has_filenames{false};
    bool has_line_numbers{false};
    bool has_inline_frames{false};
  };

  struct Mapping {
    explicit Mapping(
        const tables::StackProfileMappingTable::ConstRowReference& mapping,
        const StringPool& string_pool,
        StringTable& string_table);

    // Heuristic to determine if this maps to the main binary. Bigger scores
    // mean higher likelihood.
    int64_t ComputeMainBinaryScore() const;

    const uint64_t memory_start;
    const uint64_t memory_limit;
    const uint64_t file_offset;
    const int64_t filename;
    const int64_t build_id;

    const std::string filename_str;

    DebugInfo debug_info;
  };

  struct Function {
    struct Hash {
      size_t operator()(const Function& func) const {
        perfetto::base::Hash hasher;
        hasher.UpdateAll(func.name, func.system_name, func.filename);
        return static_cast<size_t>(hasher.digest());
      }
    };

    int64_t name;
    int64_t system_name;
    int64_t filename;

    bool operator==(const Function& other) const {
      return name == other.name && system_name == other.system_name &&
             filename == other.filename;
    }
  };

  const protozero::PackedVarInt& GetLocationIdsForCallsite(
      const CallsiteId& callsite_id);

  std::vector<Line> GetLinesForSymbolSetId(
      base::Optional<uint32_t> symbol_set_id,
      uint64_t mapping_id);

  std::vector<Line> GetLines(
      const tables::StackProfileFrameTable::ConstRowReference& frame,
      uint64_t mapping_id);

  uint64_t WriteLocationIfNeeded(const FrameId& frame_id);

  uint64_t WriteFunctionIfNeeded(
      const tables::SymbolTable::ConstRowReference& symbol,
      uint64_t mapping_id);
  uint64_t WriteFunctionIfNeeded(
      const tables::StackProfileFrameTable::ConstRowReference& frame,
      uint64_t mapping_id);

  uint64_t WriteMappingIfNeeded(
      const tables::StackProfileMappingTable::ConstRowReference& mapping);
  void WriteMappings();
  void WriteMapping(uint64_t mapping_id);
  void WriteFunctions();
  void WriteLocations();

  void WriteSampleTypes(
      const std::vector<std::pair<std::string, std::string>>& sample_types);

  void Finalize();

  Mapping& GetMapping(uint64_t mapping_id) {
    return mappings_[static_cast<size_t>(mapping_id - 1)];
  }

  // Goes over the list of staged mappings and tries to determine which is the
  // most likely main binary.
  base::Optional<uint64_t> GuessMainBinary() const;

  // Profile proto being serialized.
  protozero::HeapBuffered<third_party::perftools::profiles::pbzero::Profile>
      result_;

  TraceProcessorContext& context_;
  StringTable string_table_;

  const size_t num_sample_types_;

  bool finalized_{false};

  // Caches a CallsiteId (callstack) to the list of locations emitted to the
  // profile.
  std::unordered_map<CallsiteId, protozero::PackedVarInt> cached_location_ids_;

  // Helpers to map TraceProcessor rows to already written Profile entities
  // (their ids).
  std::unordered_map<FrameId, uint64_t> seen_locations_;
  std::unordered_map<MappingId, uint64_t> seen_mappings_;
  std::unordered_map<FrameId, uint64_t> seen_functions_;

  // Helpers to deduplicate entries. Map entity to its id. These also serve as a
  // staging area until written out to the profile proto during `Finalize`. Ids
  // are consecutive integers starting at 1. (Ids with value 0 are not allowed).
  // Ids are not unique across entities (i.e. there can be a mapping_id = 1 and
  // a function_id = 1)
  std::unordered_map<Location, uint64_t, Location::Hash> locations_;
  std::unordered_map<MappingKey, uint64_t, MappingKey::Hash> mapping_keys_;
  std::unordered_map<Function, uint64_t, Function::Hash> functions_;
  // Staging area for Mappings. mapping_id - 1 = index in the vector.
  std::vector<Mapping> mappings_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_UTIL_PROFILE_BUILDER_H_
