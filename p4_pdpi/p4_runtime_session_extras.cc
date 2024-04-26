#include "p4_pdpi/p4_runtime_session_extras.h"

#include "gutil/status.h"
#include "p4/v1/p4runtime.pb.h"
#include "p4_pdpi/ir.h"
#include "p4_pdpi/ir.pb.h"
#include "p4_pdpi/p4_runtime_session.h"
#include "p4_pdpi/pd.h"

namespace pdpi {

absl::Status InstallPdTableEntries(
    P4RuntimeSession& p4rt, const google::protobuf::Message& pd_table_entries) {
  // Convert entries to PI representation.
  ASSIGN_OR_RETURN(p4::v1::GetForwardingPipelineConfigResponse config,
                   GetForwardingPipelineConfig(&p4rt));
  ASSIGN_OR_RETURN(IrP4Info info, CreateIrP4Info(config.config().p4info()));
  ASSIGN_OR_RETURN(std::vector<p4::v1::TableEntry> pi_entries,
                   PdTableEntriesToPi(info, pd_table_entries));

  // Install entries.
  return InstallPiTableEntries(&p4rt, info, pi_entries);
}
absl::Status InstallPdTableEntry(
    P4RuntimeSession& p4rt, const google::protobuf::Message& pd_table_entry) {
  // Convert entries to PI representation.
  ASSIGN_OR_RETURN(p4::v1::GetForwardingPipelineConfigResponse config,
                   GetForwardingPipelineConfig(&p4rt));
  ASSIGN_OR_RETURN(IrP4Info info, CreateIrP4Info(config.config().p4info()));
  ASSIGN_OR_RETURN(p4::v1::TableEntry pi_entry,
                   PdTableEntryToPi(info, pd_table_entry));

  // Install entries.
  return InstallPiTableEntry(&p4rt, pi_entry);
}

}  // namespace pdpi
