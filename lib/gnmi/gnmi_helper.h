// Copyright (c) 2024, Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef PINS_LIB_GNMI_GNMI_HELPER_H_
#define PINS_LIB_GNMI_GNMI_HELPER_H_

#include <cstdint>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/numeric/int128.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "github.com/openconfig/gnoi/types/types.pb.h"
#include "lib/gnmi/openconfig.pb.h"
#include "p4_pdpi/p4_runtime_session.h"
#include "proto/gnmi/gnmi.grpc.pb.h"
#include "proto/gnmi/gnmi.pb.h"
#include "thinkit/switch.h"

namespace pins_test {

inline constexpr char kOpenconfigStr[] = "openconfig";
inline constexpr char kTarget[] = "target";

// Breakout mode is represented as vector of breakout speed.
enum class BreakoutSpeed {
  k100GB,
  k200GB,
  k400GB,
};
using BreakoutMode = std::vector<BreakoutSpeed>;
std::ostream& operator<<(std::ostream& os, const BreakoutMode& breakout);

enum class GnmiSetType : char { kUpdate, kReplace, kDelete };

enum class OperStatus {
  kUnknown,
  kUp,
  kDown,
  kTesting,
};

enum class GnmiFieldType {
  kConfig,
  kState,
};

// Describes a single interface in a gNMI config.
struct OpenConfigInterfaceDescription {
  std::string port_name;
  int port_id;
};

// `TransceiverPart` holds the `vendor` and `part_number` of the physical
// transceiver.
struct TransceiverPart {
  std::string vendor;
  std::string part_number;
  std::string rev;

  bool operator==(const TransceiverPart& other) const {
    return std::tie(vendor, part_number) ==
           std::tie(other.vendor, other.part_number);
  }
};

std::string GnmiFieldTypeToString(GnmiFieldType field_type);

// Generates an OpenConfig JSON string using the given list of `interfaces` to
// define interfaces of the given `field_type`.
std::string OpenConfigWithInterfaces(
    GnmiFieldType field_type,
    absl::Span<const OpenConfigInterfaceDescription> interfaces);

// Generates a valid, empty OpenConfig JSON string.
std::string EmptyOpenConfig();

// Builds gNMI Set Request for a given OC path, set type and set value.
// The path should be in the following format below.
// "interfaces/interface[Ethernet0]/config/mtu".
// The set value should be in the format ex: {\"mtu\":2000}.
absl::StatusOr<gnmi::SetRequest> BuildGnmiSetRequest(
    absl::string_view oc_path, GnmiSetType set_type,
    absl::string_view json_val = {});

// Builds gNMI Get Request for a given OC path.
// The path should be in the following format below.
// "interfaces/interface[Ethernet0]/config/mtu".
absl::StatusOr<gnmi::GetRequest> BuildGnmiGetRequest(
    absl::string_view oc_path, gnmi::GetRequest::DataType req_type);

// Parses Get Response to retrieve specific tag value.
absl::StatusOr<std::string> ParseGnmiGetResponse(
    const gnmi::GetResponse& response, absl::string_view match_tag = {});

absl::Status SetGnmiConfigPath(gnmi::gNMI::StubInterface* gnmi_stub,
                               absl::string_view config_path,
                               GnmiSetType operation, absl::string_view value);

absl::StatusOr<std::string> GetGnmiStatePathInfo(
    gnmi::gNMI::StubInterface* gnmi_stub, absl::string_view state_path,
    absl::string_view resp_parse_str = {});

struct ResultWithTimestamp {
  std::string response;
  int64_t timestamp;
};

absl::StatusOr<ResultWithTimestamp> GetGnmiStatePathAndTimestamp(
    gnmi::gNMI::StubInterface* gnmi_stub, absl::string_view path,
    absl::string_view resp_parse_str);

absl::StatusOr<std::string> ReadGnmiPath(gnmi::gNMI::StubInterface* gnmi_stub,
                                         absl::string_view path,
                                         gnmi::GetRequest::DataType req_type,
                                         absl::string_view resp_parse_str = {});

template <class T>
std::string ConstructGnmiConfigSetString(std::string field, T value) {
  std::string result_str;
  if (std::is_integral<T>::value) {
    // result: "{\"field\":value}"
    result_str = absl::StrCat("{\"", field, "\":", value, "}");
  } else if (std::is_same<T, std::string>::value) {
    // result: "{\"field\":\"value\"};
    result_str = absl::StrCat("{\"", field, "\":\"", value, "\"}");
  }

  return result_str;
}

// Adding subtree to gNMI Subscription list.
void AddSubtreeToGnmiSubscription(absl::string_view subtree_root,
                                  gnmi::SubscriptionList& subscription_list,
                                  gnmi::SubscriptionMode mode,
                                  bool suppress_redundant,
                                  absl::Duration interval);

// Returns vector of elements in subscriber response.
absl::StatusOr<std::vector<absl::string_view>>
GnmiGetElementFromTelemetryResponse(const gnmi::SubscribeResponse& response);

// Pushes a given gNMI config to a given chassis. This method will automatically
// configure the arbitration settings for the request.
absl::Status PushGnmiConfig(
    gnmi::gNMI::StubInterface& stub, const std::string& chassis_name,
    const std::string& gnmi_config,
    absl::uint128 election_id = pdpi::TimeBasedElectionId());

// Pushes a given gNMI config to a thinkit switch. This method will make
// sensible changes to the config like:
//    * Update the P4RT device ID to match the chassis settings.
absl::Status PushGnmiConfig(thinkit::Switch& chassis,
                            const std::string& gnmi_config);

absl::Status WaitForGnmiPortIdConvergence(gnmi::gNMI::StubInterface& stub,
                                          const std::string& gnmi_config,
                                          const absl::Duration& timeout);
absl::Status WaitForGnmiPortIdConvergence(thinkit::Switch& chassis,
                                          const std::string& gnmi_config,
                                          const absl::Duration& timeout);

absl::Status CanGetAllInterfaceOverGnmi(
    gnmi::gNMI::StubInterface& stub,
    absl::Duration timeout = absl::Seconds(60));

absl::StatusOr<gnmi::GetResponse> GetAllInterfaceOverGnmi(
    gnmi::gNMI::StubInterface& stub,
    absl::Duration timeout = absl::Seconds(60));

// Gets the interface to oper status map.
absl::StatusOr<absl::flat_hash_map<std::string, std::string>>
GetInterfaceToOperStatusMapOverGnmi(gnmi::gNMI::StubInterface& stub,
                                    absl::Duration timeout);

// Checks if given interfaces' oper-status is up/down. Passing in nothing for
// `interfaces` will check for all interfaces.
absl::Status CheckInterfaceOperStateOverGnmi(
    gnmi::gNMI::StubInterface& stub, absl::string_view interface_oper_state,
    absl::Span<const std::string> interfaces = {},
    bool skip_non_ethernet_interfaces = false,
    absl::Duration timeout = absl::Seconds(60));

// Returns gNMI Path for OC strings.
gnmi::Path ConvertOCStringToPath(absl::string_view oc_path);

// Converts from a gNMI path to a gNOI path.
gnoi::types::Path GnmiToGnoiPath(gnmi::Path path);

// Gets all the EthernetXX interfaces whose operational status is UP.
absl::StatusOr<std::vector<std::string>> GetUpInterfacesOverGnmi(
    gnmi::gNMI::StubInterface& stub,
    absl::Duration timeout = absl::Seconds(60));

// Gets the operational status of an interface.
absl::StatusOr<OperStatus> GetInterfaceOperStatusOverGnmi(
    gnmi::gNMI::StubInterface& stub, absl::string_view if_name);

// Returns the interface name to port id map from a gNMI config.
absl::StatusOr<absl::flat_hash_map<std::string, std::string>>
GetAllInterfaceNameToPortId(absl::string_view gnmi_config);

// Reads the gNMI state and returns the interface name to port id map.
absl::StatusOr<absl::flat_hash_map<std::string, std::string>>
GetAllInterfaceNameToPortId(gnmi::gNMI::StubInterface& stub);

// Gets interfaces from the switch and returns them as a proto.
absl::StatusOr<openconfig::Interfaces> GetInterfacesAsProto(
    gnmi::gNMI::StubInterface& stub, gnmi::GetRequest::DataType type,
    absl::Duration timeout = absl::Seconds(60));

// Gets interfaces satisfying `predicate` from the switch and returns them as a
// proto.
absl::StatusOr<openconfig::Interfaces> GetMatchingInterfacesAsProto(
    gnmi::gNMI::StubInterface& stub, gnmi::GetRequest::DataType type,
    std::function<bool(const openconfig::Interfaces::Interface&)> predicate,
    absl::Duration timeout = absl::Seconds(60));

// Reads the gNMI config from the switch and returns a map of all enabled
// interfaces to their p4rt port id.
absl::StatusOr<absl::flat_hash_map<std::string, std::string>>
GetAllEnabledInterfaceNameToPortId(gnmi::gNMI::StubInterface& stub,
                                   absl::Duration timeout = absl::Seconds(60));

// Checks the switch's gNMI state for any ports that are UP, and returns a map
// of the port IDs by the port names. If a port is not UP or does not have an ID
// it will not be returned.
absl::StatusOr<absl::flat_hash_map<std::string, std::string>>
GetAllUpInterfacePortIdsByName(gnmi::gNMI::StubInterface& stub,
                               absl::Duration timeout = absl::Seconds(60));

// Checks the switch's gNMI state for any port that is UP with a Port ID. If no
// port is UP with a Port ID then an error is returned.
absl::StatusOr<std::string> GetAnyUpInterfacePortId(
    gnmi::gNMI::StubInterface& stub,
    absl::Duration timeout = absl::Seconds(60));

// Checks the switch's gNMI state for N ports that are both UP and have a Port
// ID. If the switch does not have enough ports meeting this condition then an
// error is returned.
absl::StatusOr<std::vector<std::string>> GetNUpInterfacePortIds(
    gnmi::gNMI::StubInterface& stub, int num_interfaces,
    absl::Duration timeout = absl::Seconds(60));

// Deterministically modifies the config of `gnmi_stub` to map all
// `desired_p4rt_ids` to interfaces on the switch that match the given
// `predicate`. If too few interfaces match the `predicate` to map all
// `desired_p4rt_ids`, the function will fail. Any matching interface already
// mapping a desired P4RT ID will be left unchanged. Any non-matching
// interface that maps a desired P4RT ID will have the mapping removed.
absl::Status MapP4rtIdsToMatchingInterfaces(
    gnmi::gNMI::StubInterface& gnmi_stub,
    const absl::btree_set<int>& desired_p4rt_ids,
    std::function<bool(const openconfig::Interfaces::Interface&)> predicate,
    absl::Duration timeout = absl::Seconds(60));

// Uses `gnmi_stub` to set the P4RT IDs of `interfaces`, deleting any of those
// P4RT IDs previously mapped on the switch a P4RT ID can't be mapped to
// multiple interfaces.
absl::Status SetInterfaceP4rtIds(gnmi::gNMI::StubInterface& gnmi_stub,
                                 const openconfig::Interfaces& interfaces);

// Returns the ordered set of all port ids mapped by the given gNMI config.
absl::StatusOr<absl::btree_set<std::string>> GetAllPortIds(
    absl::string_view gnmi_config);

// Reads the gNMI state and returns the ordered set of all port ids mapped.
absl::StatusOr<absl::btree_set<std::string>> GetAllPortIds(
    gnmi::gNMI::StubInterface& stub);

// Gets all system process id over gNMI.
absl::StatusOr<gnmi::GetResponse> GetAllSystemProcesses(
    gnmi::gNMI::StubInterface& gnmi_stub);

// Gets system memory usage over gNMI.
absl::StatusOr<gnmi::GetResponse> GetSystemMemory(
    gnmi::gNMI::StubInterface& gnmi_stub);

// Parses the alarms JSON array returned from a gNMI Get request to
// "openconfig-system:system/alarms/alarm". Returns the list of alarms.
absl::StatusOr<std::vector<std::string>> ParseAlarms(
    const std::string& alarms_json);

// Gets alarms over gNMI.
absl::StatusOr<std::vector<std::string>> GetAlarms(
    gnmi::gNMI::StubInterface& gnmi_stub);

// Strips the beginning and ending double-quotes from the `string`.
absl::string_view StripQuotes(absl::string_view string);

// Strips the beginning and ending brackets ('[', ']') from the `string`.
absl::string_view StripBrackets(absl::string_view string);

// Returns a map from interface names to their physical transceiver name.
absl::StatusOr<absl::flat_hash_map<std::string, std::string>>
GetInterfaceToTransceiverMap(gnmi::gNMI::StubInterface& gnmi_stub);

// Returns a map from physical transceiver names to their part information.
absl::StatusOr<absl::flat_hash_map<std::string, TransceiverPart>>
GetTransceiverPartInformation(gnmi::gNMI::StubInterface& gnmi_stub);

// Returns a map from physical transceiver names to their form factor.
absl::StatusOr<absl::flat_hash_map<std::string, std::string>>
GetTransceiverToFormFactorMap(gnmi::gNMI::StubInterface& gnmi_stub);

// Sets the device ID which is needed by P4RT App to establish a connection to
// the switch.
absl::Status SetDeviceId(gnmi::gNMI::StubInterface& gnmi_stub,
                         uint32_t device_id);

// Takes a gNMI config in JSON format and updates the P4RT Device ID. Adding it
// when it doesn't exist, or updating the value if it does.
std::string UpdateDeviceIdInJsonConfig(const std::string& gnmi_config,
                                       const std::string& device_id);

// Return the port id whose breakout mode matches the given input.
// Input: the configuration's open config as string format.
// Ignore ports is optional that is set as empty as default.
absl::StatusOr<int> FindPortWithBreakoutMode(
    absl::string_view json_config, const BreakoutMode& breakout,
    const absl::flat_hash_set<int>& ignore_ports = {});

// Returns a map from physical transceiver names to ethernet PMD type.
absl::StatusOr<absl::flat_hash_map<std::string, std::string>>
GetTransceiverToEthernetPmdMap(gnmi::gNMI::StubInterface& gnmi_stub);

// Returns a map from interfaces names to the speed of each lane in the port,
// as an integer in Kbps.
// Example: for a 4-lane 200G interface, this mapping would give a lane speed of
// 50'000'000 (50G).
absl::StatusOr<absl::flat_hash_map<std::string, int>>
GetInterfaceToLaneSpeedMap(gnmi::gNMI::StubInterface& gnmi_stub,
                           absl::flat_hash_set<std::string>& interface_names);

// Check if switch port link is up.
absl::StatusOr<bool> CheckLinkUp(const std::string& interface_name,
                                 gnmi::gNMI::StubInterface& gnmi_stub);
// Set port speed using gNMI.
absl::Status SetPortSpeedInBitsPerSecond(const std::string& port_speed,
                                         const std::string& interface_name,
                                         gnmi::gNMI::StubInterface& gnmi_stub);

// Get configured port speed.
absl::StatusOr<int64_t> GetPortSpeedInBitsPerSecond(
    const std::string& interface_name, gnmi::gNMI::StubInterface& gnmi_stub);

// Set port MTU using gNMI.
absl::Status SetPortMtu(int port_mtu, const std::string& interface_name,
                        gnmi::gNMI::StubInterface& gnmi_stub);

// Set a port in loopback mode.
absl::Status SetPortLoopbackMode(bool port_loopback,
                                 absl::string_view interface_name,
                                 gnmi::gNMI::StubInterface& gnmi_stub);

// Appends sFlow config to `gnmi_config` and returns modified config if success.
// The modified config would sort collector IPs and interface names by string
// order. This function would return original `gnmi_config` if sFlow config is
// already present in `gnmi_config`. Returns an FailedPreconditionError if
// `agent_addr_ipv6` or `sflow_enabled_interfaces` is empty.
absl::StatusOr<std::string> AppendSflowConfigIfNotPresent(
    absl::string_view gnmi_config, absl::string_view agent_addr_ipv6,
    const absl::flat_hash_map<std::string, int>& collector_address_to_port,
    const absl::flat_hash_set<std::string>& sflow_enabled_interfaces,
    const int sampling_rate, const int sampling_header_size);
}  // namespace pins_test
#endif  // PINS_LIB_GNMI_GNMI_HELPER_H_
