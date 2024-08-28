// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "p4rt_app/sonic/packet_replication_entry_translation.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/btree_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "glog/logging.h"
#include "gutil/status.h"
#include "p4_pdpi/ir.pb.h"
#include "p4rt_app/sonic/app_db_to_pdpi_ir_translator.h"
#include "p4rt_app/sonic/redis_connections.h"
#include "swss/schema.h"
#include "swss/table.h"

namespace p4rt_app {
namespace sonic {
namespace {

std::string_view TablePrefix() {
  static const auto* kTablePrefix = new std::string(
      absl::StrCat(APP_P4RT_REPLICATION_IP_MULTICAST_TABLE_NAME, ":"));
  return *kTablePrefix;
}

absl::StatusOr<std::string> StripTableName(absl::string_view app_db_key) {
  const absl::string_view kTablePrefix = TablePrefix();
  if (!absl::StartsWith(app_db_key, kTablePrefix)) {
    return gutil::InvalidArgumentErrorBuilder()
           << "Invalid packet replication App DB key " << app_db_key;
  }
  return std::string{app_db_key.substr(kTablePrefix.size())};
}

std::string GetRedisPacketReplicationTableKey(
    const pdpi::IrPacketReplicationEngineEntry& entry) {
  // The final AppDb Key format is: <table_name>:<multicast_group_id>
  return absl::StrCat(TablePrefix(), IrMulticastGroupEntryToAppDbKey(
                                         entry.multicast_group_entry()));
}

std::string CreateEntryForInsert(
    const pdpi::IrPacketReplicationEngineEntry& entry,
    std::vector<swss::KeyOpFieldsValuesTuple>& p4rt_inserts) {
  std::string key = GetRedisPacketReplicationTableKey(entry);

  swss::KeyOpFieldsValuesTuple key_value;
  kfvKey(key_value) = key;
  kfvOp(key_value) = "SET";

  for (auto& r : entry.multicast_group_entry().replicas()) {
    // Since port and/or instance are not independently unique for a group, we
    // make a key here that is a combination, which is guaranteed to be unique.
    // The value "replica" is not used.
    std::string port_instance =
        absl::StrCat(r.port(), ":0x", absl::Hex(r.instance()));
    kfvFieldsValues(key_value).push_back(
        std::make_pair(port_instance, "replica"));
  }
  p4rt_inserts.push_back(std::move(key_value));
  return key;
}

std::string CreateEntryForDelete(
    const pdpi::IrPacketReplicationEngineEntry& entry,
    std::vector<swss::KeyOpFieldsValuesTuple>& p4rt_deletes) {
  std::string key = GetRedisPacketReplicationTableKey(entry);

  swss::KeyOpFieldsValuesTuple key_value;
  kfvKey(key_value) = key;
  kfvOp(key_value) = "DEL";
  p4rt_deletes.push_back(std::move(key_value));

  return key;
}

void ComparePacketReplicationEntities(const pdpi::IrEntity& entity_app_db,
                                      const pdpi::IrEntity& entity_cache,
                                      std::vector<std::string>& failures) {
  const auto& group_entry_app_db =
      entity_app_db.packet_replication_engine_entry().multicast_group_entry();
  const auto& group_entry_cache =
      entity_cache.packet_replication_engine_entry().multicast_group_entry();

  // There's no need to check the multicast group ID, since the caller only
  // attempts to compare entities with equal multicast group IDs.

  absl::btree_set<std::string> port_instance_app_db;
  for (const auto& replica : group_entry_app_db.replicas()) {
    std::string pi = absl::StrCat(replica.port(), "_", replica.instance());
    port_instance_app_db.insert(pi);
  }

  absl::btree_set<std::string> port_instance_cache;
  for (const auto& replica : group_entry_cache.replicas()) {
    std::string pi = absl::StrCat(replica.port(), "_", replica.instance());
    port_instance_cache.insert(pi);
  }

  // Check difference between App DB and the cache.
  std::vector<std::string> differences;
  std::set_difference(port_instance_app_db.begin(), port_instance_app_db.end(),
                      port_instance_cache.begin(), port_instance_cache.end(),
                      std::inserter(differences, differences.begin()));

  for (const auto& difference : differences) {
    failures.push_back(absl::StrCat(
        "Packet replication cache is missing replica ", difference,
        " for group id ", group_entry_app_db.multicast_group_id()));
  }

  // Check difference between cache and App DB.
  differences.clear();
  std::set_difference(port_instance_cache.begin(), port_instance_cache.end(),
                      port_instance_app_db.begin(), port_instance_app_db.end(),
                      std::inserter(differences, differences.begin()));

  for (const auto& difference : differences) {
    failures.push_back(absl::StrCat("APP DB is missing replica ", difference,
                                    " for group id ",
                                    group_entry_app_db.multicast_group_id()));
  }
}

}  // namespace

absl::StatusOr<std::string> CreatePacketReplicationTableUpdateForAppDb(
    P4rtTable& p4rt_table, p4::v1::Update::Type update_type,
    const pdpi::IrPacketReplicationEngineEntry& entry,
    std::vector<swss::KeyOpFieldsValuesTuple>& kfv_updates) {
  VLOG(2) << p4::v1::Update::Type_Name(update_type)
          << " PDPI IR packet replication entry: " << entry.ShortDebugString();
  std::string update_key;
  switch (update_type) {
    case p4::v1::Update::INSERT:
    case p4::v1::Update::MODIFY:
      // Modify looks exactly the same as insert.
      // The Orchagent layer resolves differences.
      update_key = CreateEntryForInsert(entry, kfv_updates);
      break;
    case p4::v1::Update::DELETE:
      update_key = CreateEntryForDelete(entry, kfv_updates);
      break;
    default:
      return gutil::InvalidArgumentErrorBuilder()
             << "Unsupported update type: " << update_type;
      break;
  }
  return update_key;
}

std::vector<std::string> GetAllPacketReplicationTableEntryKeys(
    P4rtTable& p4rt_table) {
  std::vector<std::string> pre_keys;
  for (const auto& key : p4rt_table.app_db->keys()) {
    if (absl::StartsWith(key, TablePrefix())) {
      pre_keys.push_back(key);
    }
  }
  return pre_keys;
}

absl::StatusOr<std::vector<pdpi::IrPacketReplicationEngineEntry>>
GetAllAppDbPacketReplicationTableEntries(P4rtTable& p4rt_table) {
  std::vector<pdpi::IrPacketReplicationEngineEntry> pre_entries;

  // Each key corresponds to a single multicast group, with all its replicas.
  auto keys = GetAllPacketReplicationTableEntryKeys(p4rt_table);
  for (const std::string& key : keys) {
    VLOG(1) << "Read packet replication engine entry " << key << " from App DB";
    ASSIGN_OR_RETURN(std::string multicast_group_id, StripTableName(key));

    pdpi::IrPacketReplicationEngineEntry pre_entry;
    auto* multicast_group_entry = pre_entry.mutable_multicast_group_entry();

    // Multicast Group ID.
    uint32_t group_id;
    if (absl::SimpleHexAtoi(multicast_group_id, &group_id)) {
      multicast_group_entry->set_multicast_group_id(group_id);
    } else {
      return gutil::InvalidArgumentErrorBuilder()
             << "Failed to parse multicast_group_id from App DB packet "
             << "replication entry key '" << key;
    }

    // Build replicas.
    for (auto& kfv : p4rt_table.app_db->get(key)) {
      auto value_split = kfv.first.rfind(":");
      if (value_split == std::string::npos) {
        return gutil::InvalidArgumentErrorBuilder()
               << "Unexpected multicast port/instance format '" << kfv.first
               << "' for APP DB packet replication";
      }
      std::string port = kfv.first.substr(0, value_split);
      std::string instance_str = kfv.first.substr(value_split + 1);

      uint32_t instance;
      if (!absl::SimpleHexAtoi(instance_str, &instance)) {
        return gutil::InvalidArgumentErrorBuilder()
               << "Unexpected replica instance value '" << instance_str
               << "' for APP DB packet replication";
      }

      auto* replica = multicast_group_entry->add_replicas();
      replica->set_port(port);
      replica->set_instance(instance);
      // We ignore the value in the kfv, as it is not used.
    }
    pre_entries.push_back(pre_entry);
  }
  return pre_entries;
}

std::vector<std::string> ComparePacketReplicationTableEntries(
    const std::vector<pdpi::IrEntity>& entries_app_db,
    const std::vector<pdpi::IrEntity>& entries_cache) {
  std::vector<std::string> failures;

  // Multicast group ID -> IrEntity.
  absl::btree_map<uint32_t, pdpi::IrEntity> map_app_db;
  absl::btree_map<uint32_t, pdpi::IrEntity> map_cache;

  for (const auto& entry : entries_app_db) {
    map_app_db[entry.packet_replication_engine_entry()
                   .multicast_group_entry()
                   .multicast_group_id()] = entry;
  }

  for (const auto& entry : entries_cache) {
    map_cache[entry.packet_replication_engine_entry()
                  .multicast_group_entry()
                  .multicast_group_id()] = entry;
  }

  for (const auto& id_and_entry : map_app_db) {
    if (map_cache.find(id_and_entry.first) == map_cache.end()) {
      failures.push_back(
          absl::StrCat("Packet replication cache is missing multicast group ",
                       "ID ", id_and_entry.first));
      continue;
    }
    ComparePacketReplicationEntities(id_and_entry.second,
                                     map_cache[id_and_entry.first], failures);
  }

  for (const auto& id_and_entry : map_cache) {
    if (map_app_db.find(id_and_entry.first) == map_app_db.end()) {
      failures.push_back(absl::StrCat("APP DB is missing multicast group ID ",
                                      id_and_entry.first));
    }
    // There's no need to compare entities here, since all overlapping entities
    // were checked in the previous for loop.
  }

  return failures;
}

}  // namespace sonic
}  // namespace p4rt_app
