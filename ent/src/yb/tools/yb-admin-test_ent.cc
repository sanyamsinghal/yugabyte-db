// Copyright (c) YugaByte, Inc.
//
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.

// Tests for the EE yb-admin command-line tool.

#include "yb/client/client.h"
#include "yb/client/ql-dml-test-base.h"

#include "yb/integration-tests/external_mini_cluster_ent.h"

#include "yb/master/master_backup.proxy.h"

#include "yb/rpc/secure_stream.h"

#include "yb/tools/yb-admin_util.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"

#include "yb/util/date_time.h"
#include "yb/util/env_util.h"
#include "yb/util/path_util.h"
#include "yb/util/subprocess.h"
#include "yb/util/test_util.h"

DECLARE_string(certs_dir);

namespace yb {
namespace tools {

using namespace std::literals;

using std::shared_ptr;
using std::string;

using client::YBTable;
using client::YBTableInfo;
using client::YBTableName;
using client::Transactional;
using master::ListSnapshotsRequestPB;
using master::ListSnapshotsResponsePB;
using master::ListSnapshotRestorationsRequestPB;
using master::ListSnapshotRestorationsResponsePB;
using master::MasterBackupServiceProxy;
using master::SysSnapshotEntryPB;
using rpc::RpcController;

class AdminCliTest : public client::KeyValueTableTest<MiniCluster> {
 protected:
  Result<MasterBackupServiceProxy*> BackupServiceProxy() {
    if (!backup_service_proxy_) {
      backup_service_proxy_.reset(new MasterBackupServiceProxy(
          &client_->proxy_cache(),
          VERIFY_RESULT(cluster_->GetLeaderMasterBoundRpcAddr())));
    }
    return backup_service_proxy_.get();
  }

  template <class... Args>
  Result<std::string> RunAdminToolCommand(Args&&... args) {
    return yb::RunAdminToolCommand(cluster_->GetMasterAddresses(), std::forward<Args>(args)...);
  }

  template <class... Args>
  Status RunAdminToolCommandAndGetErrorOutput(string* error_msg, Args&&... args) {
    auto command = ToStringVector(
            GetToolPath("yb-admin"), "-master_addresses", cluster_->GetMasterAddresses(),
            std::forward<Args>(args)...);
    LOG(INFO) << "Run tool: " << AsString(command);
    return Subprocess::Call(command, error_msg, StdFdTypes{StdFdType::kErr});
  }

  template <class... Args>
  Result<rapidjson::Document> RunAdminToolCommandJson(Args&&... args) {
    auto raw = VERIFY_RESULT(RunAdminToolCommand(std::forward<Args>(args)...));
    rapidjson::Document result;
    if (result.Parse(raw.c_str(), raw.length()).HasParseError()) {
      return STATUS_FORMAT(
          InvalidArgument, "Failed to parse json output $0: $1", result.GetParseError(), raw);
    }
    return result;
  }

  CHECKED_STATUS WaitForRestoreSnapshot() {
    return WaitFor([this]() -> Result<bool> {
      auto document = VERIFY_RESULT(RunAdminToolCommandJson("list_snapshot_restorations"));
      auto it = document.FindMember("restorations");
      if (it == document.MemberEnd()) {
        LOG(INFO) << "No restorations";
        return false;
      }
      auto value = it->value.GetArray();
      for (const auto& restoration : value) {
        auto state_it = restoration.FindMember("state");
        if (state_it == restoration.MemberEnd()) {
          return STATUS(NotFound, "'state' not found");
        }
        if (state_it->value.GetString() != "RESTORED"s) {
          return false;
        }
      }
      return true;
    },
    30s, "Waiting for snapshot restore to complete");
  }

  Result<ListSnapshotsResponsePB> WaitForAllSnapshots() {
    auto* proxy = VERIFY_RESULT(BackupServiceProxy());

    ListSnapshotsRequestPB req;
    ListSnapshotsResponsePB resp;
    RETURN_NOT_OK(
        WaitFor([proxy, &req, &resp]() -> Result<bool> {
                  RpcController rpc;
                  RETURN_NOT_OK(proxy->ListSnapshots(req, &resp, &rpc));
                  for (auto const& snapshot : resp.snapshots()) {
                    if (snapshot.entry().state() != SysSnapshotEntryPB::COMPLETE) {
                      return false;
                    }
                  }
                  return true;
                },
                30s, "Waiting for all snapshots to complete"));
    return resp;
  }

  Result<string> GetCompletedSnapshot(int num_snapshots = 1,
                                      int idx = 0) {
    auto resp = VERIFY_RESULT(WaitForAllSnapshots());

    if (resp.snapshots_size() != num_snapshots) {
      return STATUS_FORMAT(Corruption, "Wrong snapshot count $0", resp.snapshots_size());
    }

    return SnapshotIdToString(resp.snapshots(idx).id());
  }

  Result<SysSnapshotEntryPB::State> WaitForRestoration() {
    auto* proxy = VERIFY_RESULT(BackupServiceProxy());

    ListSnapshotRestorationsRequestPB req;
    ListSnapshotRestorationsResponsePB resp;
    RETURN_NOT_OK(
        WaitFor([proxy, &req, &resp]() -> Result<bool> {
          RpcController rpc;
          RETURN_NOT_OK(proxy->ListSnapshotRestorations(req, &resp, &rpc));
          for (auto const& restoration : resp.restorations()) {
            if (restoration.entry().state() == SysSnapshotEntryPB::RESTORING) {
              return false;
            }
          }
          return true;
        },
        30s, "Waiting for all restorations to complete"));

    SCHECK_EQ(resp.restorations_size(), 1, IllegalState, "Expected only one restoration");
    return resp.restorations(0).entry().state();
  }

  Result<size_t> NumTables(const string& table_name) const;
  void ImportTableAs(const string& snapshot_file, const string& keyspace, const string& table_name);
  void CheckImportedTable(
      const YBTable* src_table, const YBTableName& yb_table_name, bool same_ids = false);
  void CheckAndDeleteImportedTable(
      const string& keyspace, const string& table_name, bool same_ids = false);
  void CheckImportedTableWithIndex(
      const string& keyspace, const string& table_name, const string& index_name,
      bool same_ids = false);

  void DoTestImportSnapshot(const string& format = "");
  void DoTestExportImportIndexSnapshot(Transactional transactional);

 private:
  std::unique_ptr<MasterBackupServiceProxy> backup_service_proxy_;
};

TEST_F(AdminCliTest, TestNonTLS) {
  ASSERT_OK(RunAdminToolCommand("list_all_masters"));
}

// TODO: Enabled once ENG-4900 is resolved.
TEST_F(AdminCliTest, DISABLED_TestTLS) {
  const auto sub_dir = JoinPathSegments("ent", "test_certs");
  auto root_dir = env_util::GetRootDir(sub_dir) + "/../../";
  ASSERT_OK(RunAdminToolCommand(
    "--certs_dir_name", JoinPathSegments(root_dir, sub_dir), "list_all_masters"));
}

TEST_F(AdminCliTest, TestCreateSnapshot) {
  CreateTable(Transactional::kFalse);
  const string& table_name = table_.name().table_name();
  const string& keyspace = table_.name().namespace_name();

  // There is custom table.
  const auto tables = ASSERT_RESULT(client_->ListTables(table_name, /* exclude_ysql */ true));
  ASSERT_EQ(1, tables.size());

  ListSnapshotsRequestPB req;
  ListSnapshotsResponsePB resp;
  RpcController rpc;
  ASSERT_OK(ASSERT_RESULT(BackupServiceProxy())->ListSnapshots(req, &resp, &rpc));
  ASSERT_EQ(resp.snapshots_size(), 0);

  // Create snapshot of default table that gets created.
  ASSERT_OK(RunAdminToolCommand("create_snapshot", keyspace, table_name));

  rpc.Reset();
  ASSERT_OK(ASSERT_RESULT(BackupServiceProxy())->ListSnapshots(req, &resp, &rpc));
  ASSERT_EQ(resp.snapshots_size(), 1);

  LOG(INFO) << "Test finished: " << CURRENT_TEST_CASE_AND_TEST_NAME_STR();
}

Result<size_t> AdminCliTest::NumTables(const string& table_name) const {
  auto tables = VERIFY_RESULT(
      client_->ListTables(/* filter */ table_name, /* exclude_ysql */ true));
  return tables.size();
}

void AdminCliTest::CheckImportedTable(const YBTable* src_table,
                                      const YBTableName& yb_table_name,
                                      bool same_ids) {
  shared_ptr<YBTable> table;
  ASSERT_OK(client_->OpenTable(yb_table_name, &table));

  ASSERT_EQ(same_ids, table->id() == src_table->id());
  ASSERT_EQ(table->table_type(), src_table->table_type());
  ASSERT_EQ(table->GetPartitionsCopy(), src_table->GetPartitionsCopy());
  ASSERT_TRUE(table->partition_schema().Equals(src_table->partition_schema()));
  ASSERT_TRUE(table->schema().Equals(src_table->schema()));
  ASSERT_EQ(table->schema().table_properties().is_transactional(),
            src_table->schema().table_properties().is_transactional());
}

void AdminCliTest::CheckAndDeleteImportedTable(const string& keyspace,
                                               const string& table_name,
                                               bool same_ids) {
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots());

  const YBTableName yb_table_name(YQL_DATABASE_CQL, keyspace, table_name);
  CheckImportedTable(table_.get(), yb_table_name, same_ids);
  ASSERT_EQ(1, ASSERT_RESULT(NumTables(table_name)));
  ASSERT_OK(client_->DeleteTable(yb_table_name, /* wait */ true));
  ASSERT_EQ(0, ASSERT_RESULT(NumTables(table_name)));
}

void AdminCliTest::ImportTableAs(const string& snapshot_file,
                                 const string& keyspace,
                                 const string& table_name) {
  ASSERT_OK(RunAdminToolCommand("import_snapshot", snapshot_file, keyspace, table_name));
  CheckAndDeleteImportedTable(keyspace, table_name);
}

void AdminCliTest::DoTestImportSnapshot(const string& format) {
  CreateTable(Transactional::kFalse);
  const string& table_name = table_.name().table_name();
  const string& keyspace = table_.name().namespace_name();

  // Create snapshot of default table that gets created.
  ASSERT_OK(RunAdminToolCommand("create_snapshot", keyspace, table_name));
  const auto snapshot_id = ASSERT_RESULT(GetCompletedSnapshot());

  string tmp_dir;
  ASSERT_OK(Env::Default()->GetTestDirectory(&tmp_dir));
  const auto snapshot_file = JoinPathSegments(tmp_dir, "exported_snapshot.dat");

  if (format.empty()) {
    ASSERT_OK(RunAdminToolCommand("export_snapshot", snapshot_id, snapshot_file));
  } else {
    ASSERT_OK(RunAdminToolCommand("export_snapshot", snapshot_id, snapshot_file,
                                  "-TEST_metadata_file_format_version=" + format));
  }

  // Import snapshot into the existing table.
  ASSERT_OK(RunAdminToolCommand("import_snapshot", snapshot_file));
  CheckAndDeleteImportedTable(keyspace, table_name, /* same_ids */ true);

  // Import snapshot into original table from the snapshot.
  // (The table was deleted by the call above.)
  ASSERT_OK(RunAdminToolCommand("import_snapshot", snapshot_file));
  CheckAndDeleteImportedTable(keyspace, table_name);

  // Import snapshot into non existing namespace.
  ImportTableAs(snapshot_file, keyspace + "_new", table_name);
  // Import snapshot into already existing namespace.
  ImportTableAs(snapshot_file, keyspace, table_name + "_new");
  // Import snapshot into already existing namespace and table.
  ImportTableAs(snapshot_file, keyspace, table_name);
}

TEST_F(AdminCliTest, TestImportSnapshot) {
  DoTestImportSnapshot();
  LOG(INFO) << "Test finished: " << CURRENT_TEST_CASE_AND_TEST_NAME_STR();
}

TEST_F(AdminCliTest, TestImportSnapshotInOldFormat1) {
  DoTestImportSnapshot("1");
  LOG(INFO) << "Test finished: " << CURRENT_TEST_CASE_AND_TEST_NAME_STR();
}

TEST_F(AdminCliTest, TestExportImportSnapshot) {
  CreateTable(Transactional::kFalse);
  const string& table_name = table_.name().table_name();
  const string& keyspace = table_.name().namespace_name();

  // Create snapshot of default table that gets created.
  ASSERT_OK(RunAdminToolCommand("create_snapshot", keyspace, table_name));
  const auto snapshot_id = ASSERT_RESULT(GetCompletedSnapshot());

  string tmp_dir;
  ASSERT_OK(Env::Default()->GetTestDirectory(&tmp_dir));
  const auto snapshot_file = JoinPathSegments(tmp_dir, "exported_snapshot.dat");
  ASSERT_OK(RunAdminToolCommand("export_snapshot", snapshot_id, snapshot_file));
  // Import below will not create a new table - reusing the old one.
  ASSERT_OK(RunAdminToolCommand("import_snapshot", snapshot_file, keyspace, table_name));

  const YBTableName yb_table_name(YQL_DATABASE_CQL, keyspace, table_name);
  CheckImportedTable(table_.get(), yb_table_name, /* same_ids */ true);
  ASSERT_EQ(1, ASSERT_RESULT(NumTables(table_name)));

  LOG(INFO) << "Test finished: " << CURRENT_TEST_CASE_AND_TEST_NAME_STR();
}

TEST_F(AdminCliTest, TestRestoreSnapshotBasic) {
  CreateTable(Transactional::kFalse);
  const string& table_name = table_.name().table_name();
  const string& keyspace = table_.name().namespace_name();

  ASSERT_OK(WriteRow(CreateSession(), 1, 1));

  // Create snapshot of default table that gets created.
  LOG(INFO) << "Creating snapshot";
  ASSERT_OK(RunAdminToolCommand("create_snapshot", keyspace, table_name));
  const auto snapshot_id = ASSERT_RESULT(GetCompletedSnapshot());
  ASSERT_RESULT(WaitForAllSnapshots());

  ASSERT_OK(DeleteRow(CreateSession(), 1));
  ASSERT_NOK(SelectRow(CreateSession(), 1));

  // Restore snapshot into the existing table.
  LOG(INFO) << "Restoring snapshot";
  ASSERT_OK(RunAdminToolCommand("restore_snapshot", snapshot_id));
  ASSERT_OK(WaitForRestoreSnapshot());
  LOG(INFO) << "Restored snapshot";

  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    return SelectRow(CreateSession(), 1).ok();
  }, 20s, "Waiting for row from restored snapshot."));
}

TEST_F(AdminCliTest, TestRestoreSnapshotHybridTime) {
  CreateTable(Transactional::kFalse);
  const string& table_name = table_.name().table_name();
  const string& keyspace = table_.name().namespace_name();

  ASSERT_OK(WriteRow(CreateSession(), 1, 1));
  auto hybrid_time = cluster_->mini_tablet_server(0)->server()->Clock()->Now();
  ASSERT_OK(WriteRow(CreateSession(), 2, 2));

  // Create snapshot of default table that gets created.
  ASSERT_OK(RunAdminToolCommand("create_snapshot", keyspace, table_name));
  const auto snapshot_id = ASSERT_RESULT(GetCompletedSnapshot());
  ASSERT_RESULT(WaitForAllSnapshots());

  // Restore snapshot into the existing table.
  ASSERT_OK(RunAdminToolCommand("restore_snapshot", snapshot_id,
      std::to_string(hybrid_time.GetPhysicalValueMicros())));
  ASSERT_OK(WaitForRestoreSnapshot());

  // Row before HybridTime present, row after should be missing now.
  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    return SelectRow(CreateSession(), 1).ok() &&
           !SelectRow(CreateSession(), 2).ok();
  }, 20s, "Waiting for row from restored snapshot."));
}

TEST_F(AdminCliTest, TestRestoreSnapshotTimestamp) {
  CreateTable(Transactional::kFalse);
  const string& table_name = table_.name().table_name();
  const string& keyspace = table_.name().namespace_name();

  ASSERT_OK(WriteRow(CreateSession(), 1, 1));
  auto timestamp = DateTime::TimestampToString(DateTime::TimestampNow());
  LOG(INFO) << "Timestamp: " << timestamp;
  auto write_wait = 2s;
  std::this_thread::sleep_for(write_wait);
  ASSERT_OK(WriteRow(CreateSession(), 2, 2));

  // Create snapshot of default table that gets created.
  ASSERT_OK(RunAdminToolCommand("create_snapshot", keyspace, table_name));
  const auto snapshot_id = ASSERT_RESULT(GetCompletedSnapshot());
  ASSERT_RESULT(WaitForAllSnapshots());

  // Restore snapshot into the existing table.
  ASSERT_OK(RunAdminToolCommand("restore_snapshot", snapshot_id, timestamp));
  ASSERT_OK(WaitForRestoreSnapshot());

  // Row before Timestamp present, row after should be missing now.
  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    return SelectRow(CreateSession(), 1).ok() &&
           !SelectRow(CreateSession(), 2).ok();
  }, 20s, "Waiting for row from restored snapshot."));
}

TEST_F(AdminCliTest, TestRestoreSnapshotInterval) {
  CreateTable(Transactional::kFalse);
  const string& table_name = table_.name().table_name();
  const string& keyspace = table_.name().namespace_name();

  auto clock = cluster_->mini_tablet_server(0)->server()->Clock();
  ASSERT_OK(WriteRow(CreateSession(), 1, 1));
  auto pre_sleep_ht = clock->Now();
  auto write_wait = 5s;
  std::this_thread::sleep_for(write_wait);
  ASSERT_OK(WriteRow(CreateSession(), 2, 2));

  // Create snapshot of default table that gets created.
  ASSERT_OK(RunAdminToolCommand("create_snapshot", keyspace, table_name));
  const auto snapshot_id = ASSERT_RESULT(GetCompletedSnapshot());
  ASSERT_RESULT(WaitForAllSnapshots());

  // Restore snapshot into the existing table.
  auto restore_ht = clock->Now();
  auto interval = restore_ht.GetPhysicalValueMicros() - pre_sleep_ht.GetPhysicalValueMicros();
  auto i_str = std::to_string(interval/1000000) + "s";
  ASSERT_OK(RunAdminToolCommand("restore_snapshot", snapshot_id, "minus", i_str));
  ASSERT_OK(WaitForRestoreSnapshot());

  ASSERT_OK(SelectRow(CreateSession(), 1));
  auto select2 = SelectRow(CreateSession(), 2);
  ASSERT_NOK(select2);
}

void AdminCliTest::CheckImportedTableWithIndex(const string& keyspace,
                                               const string& table_name,
                                               const string& index_name,
                                               bool same_ids) {
  const YBTableName yb_table_name(YQL_DATABASE_CQL, keyspace, table_name);
  const YBTableName yb_index_name(YQL_DATABASE_CQL, keyspace, index_name);

  CheckImportedTable(table_.get(), yb_table_name, same_ids);
  ASSERT_EQ(2, ASSERT_RESULT(NumTables(table_name)));
  CheckImportedTable(index_.get(), yb_index_name, same_ids);
  ASSERT_EQ(1, ASSERT_RESULT(NumTables(index_name)));

  YBTableInfo table_info = ASSERT_RESULT(client_->GetYBTableInfo(yb_table_name));
  YBTableInfo index_info = ASSERT_RESULT(client_->GetYBTableInfo(yb_index_name));
  // Check index ---> table relation.
  ASSERT_EQ(index_info.index_info->indexed_table_id(), table_info.table_id);
  // Check table ---> index relation.
  ASSERT_EQ(table_info.index_map.size(), 1);
  ASSERT_EQ(table_info.index_map.count(index_info.table_id), 1);
  ASSERT_EQ(table_info.index_map.begin()->first, index_info.table_id);
  ASSERT_EQ(table_info.index_map.begin()->second.table_id(), index_info.table_id);
  ASSERT_EQ(table_info.index_map.begin()->second.indexed_table_id(), table_info.table_id);

  ASSERT_OK(client_->DeleteTable(yb_table_name, /* wait */ true));
  ASSERT_EQ(0, ASSERT_RESULT(NumTables(table_name)));
}

void AdminCliTest::DoTestExportImportIndexSnapshot(Transactional transactional) {
  CreateTable(transactional);
  CreateIndex(transactional);

  // Default tables that were created.
  const string& table_name = table_.name().table_name();
  const string& keyspace = table_.name().namespace_name();
  const string& index_name = index_.name().table_name();
  const YBTableName yb_table_name(YQL_DATABASE_CQL, keyspace, table_name);
  const YBTableName yb_index_name(YQL_DATABASE_CQL, keyspace, index_name);

  // Check there are 2 tables.
  ASSERT_EQ(2, ASSERT_RESULT(NumTables(table_name)));

  // Create snapshot of default table and the attached index that gets created.
  ASSERT_OK(RunAdminToolCommand("create_snapshot", keyspace, table_name));
  auto snapshot_id = ASSERT_RESULT(GetCompletedSnapshot());

  string tmp_dir;
  ASSERT_OK(Env::Default()->GetTestDirectory(&tmp_dir));
  const auto snapshot_file = JoinPathSegments(tmp_dir, "exported_snapshot.dat");
  ASSERT_OK(RunAdminToolCommand("export_snapshot", snapshot_id, snapshot_file));

  // Import table and index into the existing table and index.
  ASSERT_OK(RunAdminToolCommand("import_snapshot", snapshot_file));
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots());
  CheckImportedTableWithIndex(keyspace, table_name, index_name, /* same_ids */ true);

  // Import table and index with original names - not providing any names.
  // (The table was deleted by the call above.)
  ASSERT_OK(RunAdminToolCommand("import_snapshot", snapshot_file));
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots());
  CheckImportedTableWithIndex(keyspace, table_name, index_name);

  // Import table and index with original names - using the old names.
  ASSERT_OK(RunAdminToolCommand(
      "import_snapshot", snapshot_file, keyspace, table_name, index_name));
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots());
  CheckImportedTableWithIndex(keyspace, table_name, index_name);

  // Import table and index with original names - providing only old table name.
  ASSERT_OK(RunAdminToolCommand("import_snapshot", snapshot_file, keyspace, table_name));
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots());
  CheckImportedTableWithIndex(keyspace, table_name, index_name);

  // Renaming table and index, but keeping the same keyspace.
  ASSERT_OK(RunAdminToolCommand(
      "import_snapshot", snapshot_file, keyspace, "new_" + table_name, "new_" + index_name));
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots());
  CheckImportedTableWithIndex(keyspace, "new_" + table_name, "new_" + index_name);

  // Keeping the same table and index names, but renaming the keyspace.
  ASSERT_OK(RunAdminToolCommand("import_snapshot", snapshot_file, "new_" + keyspace));
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots());
  CheckImportedTableWithIndex("new_" + keyspace, table_name, index_name);

  // Repeat previous keyspace renaming case, but pass explicitly the same table name
  // (and skip index name).
  ASSERT_OK(RunAdminToolCommand("import_snapshot", snapshot_file, "new_" + keyspace, table_name));
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots());
  CheckImportedTableWithIndex("new_" + keyspace, table_name, index_name);

  // Import table and index into a new keyspace with old table and index names.
  ASSERT_OK(RunAdminToolCommand(
      "import_snapshot", snapshot_file, "new_" + keyspace, table_name, index_name));
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots());
  CheckImportedTableWithIndex("new_" + keyspace, table_name, index_name);

  // Rename only index and keyspace, but keep the main table name.
  ASSERT_OK(RunAdminToolCommand(
      "import_snapshot", snapshot_file, "new_" + keyspace, table_name, "new_" + index_name));
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots());
  CheckImportedTableWithIndex("new_" + keyspace, table_name, "new_" + index_name);

  // Import table and index with renaming into a new keyspace.
  ASSERT_OK(RunAdminToolCommand(
      "import_snapshot", snapshot_file, "new_" + keyspace,
      "new_" + table_name, "new_" + index_name));
  // Wait for the new snapshot completion.
  ASSERT_RESULT(WaitForAllSnapshots());
  CheckImportedTableWithIndex("new_" + keyspace, "new_" + table_name, "new_" + index_name);

  // Renaming table only, no new name for the index - expecting error.
  ASSERT_NOK(RunAdminToolCommand(
      "import_snapshot", snapshot_file, keyspace, "new_" + table_name));
  ASSERT_NOK(RunAdminToolCommand(
      "import_snapshot", snapshot_file, "new_" + keyspace, "new_" + table_name));
}

TEST_F(AdminCliTest, TestExportImportIndexSnapshot) {
  // Test non-transactional table.
  DoTestExportImportIndexSnapshot(Transactional::kFalse);
  LOG(INFO) << "Test finished: " << CURRENT_TEST_CASE_AND_TEST_NAME_STR();
}

TEST_F(AdminCliTest, TestExportImportIndexSnapshot_ForTransactional) {
  // Test the recreated transactional table.
  DoTestExportImportIndexSnapshot(Transactional::kTrue);
  LOG(INFO) << "Test finished: " << CURRENT_TEST_CASE_AND_TEST_NAME_STR();
}

TEST_F(AdminCliTest, TestFailedRestoration) {
  CreateTable(Transactional::kTrue);
  const string& table_name = table_.name().table_name();
  const string& keyspace = table_.name().namespace_name();

  // Create snapshot of default table that gets created.
  ASSERT_OK(RunAdminToolCommand("create_snapshot", keyspace, table_name));
  const auto snapshot_id = ASSERT_RESULT(GetCompletedSnapshot());
  LOG(INFO) << "Created snapshot: " << snapshot_id;

  string tmp_dir;
  ASSERT_OK(Env::Default()->GetTestDirectory(&tmp_dir));
  const auto snapshot_file = JoinPathSegments(tmp_dir, "exported_snapshot.dat");
  ASSERT_OK(RunAdminToolCommand("export_snapshot", snapshot_id, snapshot_file));
  // Import below will not create a new table - reusing the old one.
  ASSERT_OK(RunAdminToolCommand("import_snapshot", snapshot_file));

  const YBTableName yb_table_name(YQL_DATABASE_CQL, keyspace, table_name);
  CheckImportedTable(table_.get(), yb_table_name, /* same_ids */ true);
  ASSERT_EQ(1, ASSERT_RESULT(NumTables(table_name)));

  auto new_snapshot_id = ASSERT_RESULT(GetCompletedSnapshot(2));
  if (new_snapshot_id == snapshot_id) {
    new_snapshot_id = ASSERT_RESULT(GetCompletedSnapshot(2, 1));
  }
  LOG(INFO) << "Imported snapshot: " << new_snapshot_id;

  ASSERT_OK(RunAdminToolCommand("restore_snapshot", new_snapshot_id));

  const SysSnapshotEntryPB::State state = ASSERT_RESULT(WaitForRestoration());
  LOG(INFO) << "Restoration: " << SysSnapshotEntryPB::State_Name(state);
  ASSERT_EQ(state, SysSnapshotEntryPB::FAILED);

  LOG(INFO) << "Test finished: " << CURRENT_TEST_CASE_AND_TEST_NAME_STR();
}

// Configures two clusters with clients for the producer and consumer side of xcluster replication.
class XClusterAdminCliTest : public AdminCliTest {
 public:
  void SetUp() override {
    // Setup the default cluster as the consumer cluster.
    AdminCliTest::SetUp();
    // Only create a table on the consumer, producer table may differ in tests.
    CreateTable(Transactional::kTrue);

    // Create the producer cluster.
    opts.num_tablet_servers = 3;
    opts.cluster_id = kProducerClusterId;
    producer_cluster_ = std::make_unique<MiniCluster>(opts);
    ASSERT_OK(producer_cluster_->StartSync());
    ASSERT_OK(producer_cluster_->WaitForTabletServerCount(3));
    producer_cluster_client_ = ASSERT_RESULT(producer_cluster_->CreateClient());
  }

  void DoTearDown() override {
    if (producer_cluster_) {
      producer_cluster_->Shutdown();
    }
    AdminCliTest::DoTearDown();
  }

 protected:
  Status CheckTableIsBeingReplicated(const std::vector<TableId>& tables) {
    string output = VERIFY_RESULT(yb::RunAdminToolCommand(producer_cluster_->GetMasterAddresses(),
                                                          "list_cdc_streams"));
    for (const auto& table_id : tables) {
      if (output.find(table_id) == string::npos) {
        return STATUS_FORMAT(NotFound, "Table id '$0' not found in output: $1", table_id, output);
      }
    }
    return Status::OK();
  }

  const string kProducerClusterId = "producer";
  std::unique_ptr<client::YBClient> producer_cluster_client_;
  std::unique_ptr<MiniCluster> producer_cluster_;
  MiniClusterOptions opts;
};

TEST_F(XClusterAdminCliTest, TestSetupUniverseReplication) {
  client::TableHandle producer_cluster_table;

  // Create an identical table on the producer.
  client::kv_table_test::CreateTable(
      Transactional::kTrue, NumTablets(), producer_cluster_client_.get(), &producer_cluster_table);

  // Setup universe replication, this should only return once complete.
  ASSERT_OK(RunAdminToolCommand("setup_universe_replication",
                                kProducerClusterId,
                                producer_cluster_->GetMasterAddresses(),
                                producer_cluster_table->id()));

  // Check that the stream was properly created for this table.
  ASSERT_OK(CheckTableIsBeingReplicated({producer_cluster_table->id()}));

  // Delete this universe so shutdown can proceed.
  ASSERT_OK(RunAdminToolCommand("delete_universe_replication", kProducerClusterId));
}

TEST_F(XClusterAdminCliTest, TestSetupUniverseReplicationFailsWithInvalidSchema) {
  client::TableHandle producer_cluster_table;

  // Create a table with a different schema on the producer.
  client::kv_table_test::CreateTable(Transactional::kFalse, // Results in different schema!
                                     NumTablets(),
                                     producer_cluster_client_.get(),
                                     &producer_cluster_table);

  // Try to setup universe replication, should return with a useful error.
  string error_msg;
  // First provide a non-existant table id.
  // ASSERT_NOK since this should fail.
  ASSERT_NOK(RunAdminToolCommandAndGetErrorOutput(&error_msg,
                                                  "setup_universe_replication",
                                                  kProducerClusterId,
                                                  producer_cluster_->GetMasterAddresses(),
                                                  producer_cluster_table->id() + "-BAD"));

  // Verify that error message has relevant information.
  ASSERT_TRUE(error_msg.find(producer_cluster_table->id() + "-BAD not found") != string::npos);

  // Delete this universe info so we can try again.
  ASSERT_OK(RunAdminToolCommand("delete_universe_replication", kProducerClusterId));

  // Now try with the correct table id.
  ASSERT_NOK(RunAdminToolCommandAndGetErrorOutput(&error_msg,
                                                  "setup_universe_replication",
                                                  kProducerClusterId,
                                                  producer_cluster_->GetMasterAddresses(),
                                                  producer_cluster_table->id()));

  // Verify that error message has relevant information.
  ASSERT_TRUE(error_msg.find("Source and target schemas don't match") != string::npos);
}

TEST_F(XClusterAdminCliTest, TestSetupUniverseReplicationFailsWithInvalidBootstrapId) {
  client::TableHandle producer_cluster_table;

  // Create an identical table on the producer.
  client::kv_table_test::CreateTable(
      Transactional::kTrue, NumTablets(), producer_cluster_client_.get(), &producer_cluster_table);

  // Try to setup universe replication with a fake bootstrap id, should return with a useful error.
  string error_msg;
  // ASSERT_NOK since this should fail.
  ASSERT_NOK(RunAdminToolCommandAndGetErrorOutput(&error_msg,
                                                  "setup_universe_replication",
                                                  kProducerClusterId,
                                                  producer_cluster_->GetMasterAddresses(),
                                                  producer_cluster_table->id(),
                                                  "fake-bootstrap-id"));

  // Verify that error message has relevant information.
  ASSERT_TRUE(error_msg.find(
      "Could not find CDC stream: stream_id: \"fake-bootstrap-id\"") != string::npos);
}

class XClusterAlterUniverseAdminCliTest : public XClusterAdminCliTest {
 public:
  void SetUp() override {
    // Use more masters so we can test set_master_addresses
    opts.num_masters = 3;

    XClusterAdminCliTest::SetUp();
  }
};

TEST_F(XClusterAlterUniverseAdminCliTest, TestAlterUniverseReplication) {
  YB_SKIP_TEST_IN_TSAN();
  client::TableHandle producer_table;

  // Create an identical table on the producer.
  client::kv_table_test::CreateTable(
      Transactional::kTrue, NumTablets(), producer_cluster_client_.get(), &producer_table);

  // Create an additional table to test with as well.
  const YBTableName kTableName2(YQL_DATABASE_CQL, "my_keyspace", "ql_client_test_table2");
  client::TableHandle consumer_table2;
  client::TableHandle producer_table2;
  client::kv_table_test::CreateTable(
      Transactional::kTrue, NumTablets(), client_.get(), &consumer_table2, kTableName2);
  client::kv_table_test::CreateTable(
      Transactional::kTrue, NumTablets(), producer_cluster_client_.get(), &producer_table2,
      kTableName2);

  // Setup replication with both tables, this should only return once complete.
  // Only use the leader master address initially.
  ASSERT_OK(RunAdminToolCommand(
      "setup_universe_replication",
      kProducerClusterId,
      ASSERT_RESULT(producer_cluster_->GetLeaderMiniMaster())->bound_rpc_addr_str(),
      producer_table->id() + "," + producer_table2->id()));

  // Test set_master_addresses, use all the master addresses now.
  ASSERT_OK(RunAdminToolCommand("alter_universe_replication",
                                kProducerClusterId,
                                "set_master_addresses",
                                producer_cluster_->GetMasterAddresses()));
  ASSERT_OK(CheckTableIsBeingReplicated({producer_table->id(), producer_table2->id()}));

  // Test removing a table.
  ASSERT_OK(RunAdminToolCommand("alter_universe_replication",
                                kProducerClusterId,
                                "remove_table",
                                producer_table->id()));
  ASSERT_OK(CheckTableIsBeingReplicated({producer_table2->id()}));
  ASSERT_NOK(CheckTableIsBeingReplicated({producer_table->id()}));

  // Test adding a table.
  ASSERT_OK(RunAdminToolCommand("alter_universe_replication",
                                kProducerClusterId,
                                "add_table",
                                producer_table->id()));
  ASSERT_OK(CheckTableIsBeingReplicated({producer_table->id(), producer_table2->id()}));

  ASSERT_OK(RunAdminToolCommand("delete_universe_replication", kProducerClusterId));
}

}  // namespace tools
}  // namespace yb
