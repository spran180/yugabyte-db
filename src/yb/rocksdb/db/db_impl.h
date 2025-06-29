//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once

#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <list>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "yb/gutil/thread_annotations.h"

#include "yb/rocksdb/db.h"
#include "yb/rocksdb/db/column_family.h"
#include "yb/rocksdb/db/compaction.h"
#include "yb/rocksdb/db/dbformat.h"
#include "yb/rocksdb/db/flush_scheduler.h"
#include "yb/rocksdb/db/internal_stats.h"
#include "yb/rocksdb/db/log_writer.h"
#include "yb/rocksdb/db/memtable_list.h"
#include "yb/rocksdb/db/snapshot_impl.h"
#include "yb/rocksdb/db/version_edit.h"
#include "yb/rocksdb/db/wal_manager.h"
#include "yb/rocksdb/db/write_controller.h"
#include "yb/rocksdb/db/write_thread.h"
#include "yb/rocksdb/db/writebuffer.h"
#include "yb/rocksdb/env.h"
#include "yb/rocksdb/listener.h"
#include "yb/rocksdb/memtablerep.h"
#include "yb/rocksdb/port/port.h"
#include "yb/rocksdb/transaction_log.h"
#include "yb/rocksdb/util/autovector.h"
#include "yb/rocksdb/util/event_logger.h"
#include "yb/rocksdb/util/instrumented_mutex.h"
#include "yb/rocksdb/util/stop_watch.h"
#include "yb/rocksdb/util/thread_local.h"

namespace rocksdb {

class MemTable;
class TableCache;
class Version;
class VersionEdit;
class VersionSet;
class Arena;
class WriteCallback;
class FileNumbersProvider;
struct JobContext;
struct ExternalSstFileInfo;
struct RocksDBPriorityThreadPoolMetrics;

namespace internal {

constexpr int kTopDiskCompactionPriority = 100;
constexpr int kShuttingDownPriority = 200;

} // namespace internal

class DBImpl : public DB {
 public:
  DBImpl(const DBOptions& options, const std::string& dbname);
  virtual ~DBImpl();

  // Implementations of the DB interface
  using DB::Put;
  virtual Status Put(const WriteOptions& options,
                     ColumnFamilyHandle* column_family, const Slice& key,
                     const Slice& value) override;
  using DB::Merge;
  virtual Status Merge(const WriteOptions& options,
                       ColumnFamilyHandle* column_family, const Slice& key,
                       const Slice& value) override;
  using DB::Delete;
  virtual Status Delete(const WriteOptions& options,
                        ColumnFamilyHandle* column_family,
                        const Slice& key) override;
  using DB::SingleDelete;
  virtual Status SingleDelete(const WriteOptions& options,
                              ColumnFamilyHandle* column_family,
                              const Slice& key) override;
  using DB::Write;
  virtual Status Write(const WriteOptions& options,
                       WriteBatch* updates) override;

  using DB::Get;
  virtual Status Get(const ReadOptions& options,
                     ColumnFamilyHandle* column_family, const Slice& key,
                     std::string* value) override;
  using DB::MultiGet;
  virtual std::vector<Status> MultiGet(
      const ReadOptions& options,
      const std::vector<ColumnFamilyHandle*>& column_family,
      const std::vector<Slice>& keys,
      std::vector<std::string>* values) override;

  virtual Status CreateColumnFamily(const ColumnFamilyOptions& options,
                                    const std::string& column_family,
                                    ColumnFamilyHandle** handle) override;
  virtual Status DropColumnFamily(ColumnFamilyHandle* column_family) override;

  // Returns false if key doesn't exist in the database and true if it may.
  // If value_found is not passed in as null, then return the value if found in
  // memory. On return, if value was found, then value_found will be set to true
  // , otherwise false.
  using DB::KeyMayExist;
  virtual bool KeyMayExist(const ReadOptions& options,
                           ColumnFamilyHandle* column_family, const Slice& key,
                           std::string* value,
                           bool* value_found = nullptr) override;
  using DB::NewIterator;
  virtual Iterator* NewIterator(const ReadOptions& options,
                                ColumnFamilyHandle* column_family) override;
  std::unique_ptr<Iterator> NewIndexIterator(
      const ReadOptions& options, SkipLastEntry skip_last_index_entry,
      ColumnFamilyHandle* column_family) override;
  std::unique_ptr<DataBlockAwareIndexIterator> NewDataBlockAwareIndexIterator(
      const ReadOptions& options, SkipLastEntry skip_last_index_entry,
      ColumnFamilyHandle* column_family) override;
  virtual Status NewIterators(
      const ReadOptions& options,
      const std::vector<ColumnFamilyHandle*>& column_families,
      std::vector<Iterator*>* iterators) override;
  virtual const Snapshot* GetSnapshot() override;
  virtual void ReleaseSnapshot(const Snapshot* snapshot) override;
  using DB::GetProperty;
  virtual bool GetProperty(ColumnFamilyHandle* column_family,
                           const Slice& property, std::string* value) override;
  using DB::GetIntProperty;
  virtual bool GetIntProperty(ColumnFamilyHandle* column_family,
                              const Slice& property, uint64_t* value) override;
  using DB::GetAggregatedIntProperty;
  virtual bool GetAggregatedIntProperty(const Slice& property,
                                        uint64_t* aggregated_value) override;
  using DB::GetApproximateSizes;
  virtual void GetApproximateSizes(ColumnFamilyHandle* column_family,
                                   const Range* range, int n, uint64_t* sizes,
                                   bool include_memtable = false) override;
  using DB::CompactRange;
  virtual Status CompactRange(const CompactRangeOptions& options,
                              ColumnFamilyHandle* column_family,
                              const Slice* begin, const Slice* end) override;

  using DB::CompactFiles;
  virtual Status CompactFiles(const CompactionOptions& compact_options,
                              ColumnFamilyHandle* column_family,
                              const std::vector<std::string>& input_file_names,
                              const int output_level,
                              const int output_path_id = -1) override;

  virtual Status PauseBackgroundWork() override;
  virtual Status ContinueBackgroundWork() override;

  virtual Status EnableAutoCompaction(
      const std::vector<ColumnFamilyHandle*>& column_family_handles) override;

  using DB::SetOptions;
  Status SetOptions(
      ColumnFamilyHandle* column_family,
      const std::unordered_map<std::string, std::string>& options_map,
      bool dump_options = true) override;

  // Set whether DB should be flushed on shutdown.
  void SetDisableFlushOnShutdown(bool disable_flush_on_shutdown) override;
  void StartShutdown() override;

  using DB::NumberLevels;
  virtual int NumberLevels(ColumnFamilyHandle* column_family) override;
  using DB::MaxMemCompactionLevel;
  virtual int MaxMemCompactionLevel(ColumnFamilyHandle* column_family) override;
  using DB::Level0StopWriteTrigger;
  virtual int Level0StopWriteTrigger(
      ColumnFamilyHandle* column_family) override;
  virtual const std::string& GetName() const override;
  virtual Env* GetEnv() const override;
  Env* GetCheckpointEnv() const override;
  using DB::GetOptions;
  virtual const Options& GetOptions(
      ColumnFamilyHandle* column_family) const override;
  using DB::GetDBOptions;
  virtual const DBOptions& GetDBOptions() const override;
  using DB::Flush;
  virtual Status Flush(const FlushOptions& options,
                       ColumnFamilyHandle* column_family) override;
  using DB::WaitForFlush;
  virtual Status WaitForFlush(ColumnFamilyHandle* column_family) override;
  virtual Status SyncWAL() override;

  virtual SequenceNumber GetLatestSequenceNumber() const override;

  uint64_t GetNextFileNumber() const override;

  virtual Status DisableFileDeletions() override;
  virtual Status EnableFileDeletions(bool force) override;
  virtual int IsFileDeletionsEnabled() const;
  // All the returned filenames start with "/"
  virtual Status GetLiveFiles(std::vector<std::string>&,
                              uint64_t* manifest_file_size,
                              bool flush_memtable = true) override;
  virtual Status GetSortedWalFiles(VectorLogPtr* files) override;

  virtual Status GetUpdatesSince(
      SequenceNumber seq_number, std::unique_ptr<TransactionLogIterator>* iter,
      const TransactionLogIterator::ReadOptions&
          read_options = TransactionLogIterator::ReadOptions()) override;
  virtual Status DeleteFile(std::string name) override;
  Status DeleteFilesInRange(ColumnFamilyHandle* column_family,
                            const Slice* begin, const Slice* end);

  void GetLiveFilesMetaData(std::vector<LiveFileMetaData>* metadata) override;

  UserFrontierPtr GetFlushedFrontier() override;

  Status ModifyFlushedFrontier(
      UserFrontierPtr frontier,
      FrontierModificationMode mode) override;

  FlushAbility GetFlushAbility() override;

  UserFrontierPtr GetMutableMemTableFrontier(UpdateUserValueType type) override;

  // Calculates specified frontier_type for all mem tables (active and immutable).
  UserFrontierPtr CalcMemTableFrontier(UpdateUserValueType frontier_type) override;

  // Obtains the meta data of the specified column family of the DB.
  // STATUS(NotFound, "") will be returned if the current DB does not have
  // any column family match the specified name.
  // TODO(yhchiang): output parameter is placed in the end in this codebase.
  virtual void GetColumnFamilyMetaData(
      ColumnFamilyHandle* column_family,
      ColumnFamilyMetaData* metadata) override;

  // Obtains all column family options and corresponding names,
  // dropped columns are not included into the resulting collections.
  virtual void GetColumnFamiliesOptions(
      std::vector<std::string>* column_family_names,
      std::vector<ColumnFamilyOptions>* column_family_options) override;

  // experimental API
  Status SuggestCompactRange(ColumnFamilyHandle* column_family,
                             const Slice* begin, const Slice* end);

  Status PromoteL0(ColumnFamilyHandle* column_family, int target_level);

  // Similar to Write() but will call the callback once on the single write
  // thread to determine whether it is safe to perform the write.
  virtual Status WriteWithCallback(const WriteOptions& write_options,
                                   WriteBatch* my_batch,
                                   WriteCallback* callback);

  // Returns the sequence number that is guaranteed to be smaller than or equal
  // to the sequence number of any key that could be inserted into the current
  // memtables. It can then be assumed that any write with a larger(or equal)
  // sequence number will be present in this memtable or a later memtable.
  //
  // If the earliest sequence number could not be determined,
  // kMaxSequenceNumber will be returned.
  //
  // If include_history=true, will also search Memtables in MemTableList
  // History.
  SequenceNumber GetEarliestMemTableSequenceNumber(SuperVersion* sv,
                                                   bool include_history);

  // For a given key, check to see if there are any records for this key
  // in the memtables, including memtable history.  If cache_only is false,
  // SST files will also be checked.
  //
  // If a key is found, *found_record_for_key will be set to true and
  // *seq will will be set to the stored sequence number for the latest
  // operation on this key or kMaxSequenceNumber if unknown.
  // If no key is found, *found_record_for_key will be set to false.
  //
  // Note: If cache_only=false, it is possible for *seq to be set to 0 if
  // the sequence number has been cleared from the record.  If the caller is
  // holding an active db snapshot, we know the missing sequence must be less
  // than the snapshot's sequence number (sequence numbers are only cleared
  // when there are no earlier active snapshots).
  //
  // If NotFound is returned and found_record_for_key is set to false, then no
  // record for this key was found.  If the caller is holding an active db
  // snapshot, we know that no key could have existing after this snapshot
  // (since we do not compact keys that have an earlier snapshot).
  //
  // Returns OK or NotFound on success,
  // other status on unexpected error.
  Status GetLatestSequenceForKey(SuperVersion* sv, const Slice& key,
                                 bool cache_only, SequenceNumber* seq,
                                 bool* found_record_for_key);

  using DB::AddFile;
  virtual Status AddFile(ColumnFamilyHandle* column_family,
                         const ExternalSstFileInfo* file_info,
                         bool move_file) override;
  virtual Status AddFile(ColumnFamilyHandle* column_family,
                         const std::string& file_path, bool move_file) override;


  // Similar to GetSnapshot(), but also lets the db know that this snapshot
  // will be used for transaction write-conflict checking.  The DB can then
  // make sure not to compact any keys that would prevent a write-conflict from
  // being detected.
  const Snapshot* GetSnapshotForWriteConflictBoundary();

  // checks if all live files exist on file system and that their file sizes
  // match to our in-memory records
  virtual Status CheckConsistency();

  virtual Status GetDbIdentity(std::string* identity) const override;

  Status RunManualCompaction(
      ColumnFamilyData* cfd, int input_level, int output_level,
      const CompactRangeOptions& options, const Slice* begin, const Slice* end,
      bool disallow_trivial_move = false);

  // Return an internal iterator over the current state of the database.
  // The keys of this iterator are internal keys (see format.h).
  // The returned iterator should be deleted when no longer needed.
  InternalIterator* NewInternalIterator(
      Arena* arena, ColumnFamilyHandle* column_family = nullptr);

  // Extra methods (for testing) that are not in the public DB interface
  // Implemented in db_impl_debug.cc

  // Compact any files in the named level that overlap [*begin, *end]
  Status TEST_CompactRange(int level, const Slice* begin, const Slice* end,
                           ColumnFamilyHandle* column_family = nullptr,
                           bool disallow_trivial_move = false);

  // Force current memtable contents to be flushed.
  Status TEST_FlushMemTable(bool wait = true);

  // Wait for memtable compaction
  Status TEST_WaitForFlushMemTable(ColumnFamilyHandle* column_family = nullptr);

  // Wait for any compaction
  Status TEST_WaitForCompact();

  // Return the maximum overlapping data (in bytes) at next level for any
  // file at a level >= 1.
  int64_t TEST_MaxNextLevelOverlappingBytes(ColumnFamilyHandle* column_family =
                                                nullptr);

  // Return the current manifest file no.
  uint64_t TEST_Current_Manifest_FileNo();

  // get total level0 file size. Only for testing.
  uint64_t TEST_GetLevel0TotalSize();

  int TEST_NumRunningLargeCompactions();

  int TEST_NumTotalRunningCompactions();

  size_t TEST_NumNotStartedCompactionsUnlocked(CompactionSizeKind compaction_size_kind);

  int TEST_NumRunningFlushes();

  int TEST_NumBackgroundCompactionsScheduled();

  void TEST_GetFilesMetaData(ColumnFamilyHandle* column_family,
                             std::vector<std::vector<FileMetaData>>* metadata);

  void TEST_LockMutex();

  void TEST_UnlockMutex();

  InstrumentedMutex* TEST_mutex() {
    return &mutex_;
  }

  // REQUIRES: mutex locked
  void* TEST_BeginWrite();

  // REQUIRES: mutex locked
  // pass the pointer that you got from TEST_BeginWrite()
  void TEST_EndWrite(void* w);

  uint64_t TEST_MaxTotalInMemoryState() const {
    return max_total_in_memory_state_;
  }

  size_t TEST_LogsToFreeSize();

  uint64_t TEST_LogfileNumber();

  // Returns column family name to ImmutableCFOptions map.
  Status TEST_GetAllImmutableCFOptions(
      std::unordered_map<std::string, const ImmutableCFOptions*>* iopts_map);

  Cache* TEST_table_cache() { return table_cache_.get(); }

  WriteController& TEST_write_controler() { return write_controller_; }

  // Return maximum background compaction alowed to be scheduled based on
  // compaction status.
  int BGCompactionsAllowed() const;

  // Returns the list of live files in 'live' and the list
  // of all files in the filesystem in 'candidate_files'.
  // If force == false and the last call was less than
  // db_options_.delete_obsolete_files_period_micros microseconds ago,
  // it will not fill up the job_context
  void FindObsoleteFiles(JobContext* job_context, bool force,
                         bool no_full_scan = false);

  // Diffs the files listed in filenames and those that do not
  // belong to live files are posibly removed. Also, removes all the
  // files in sst_delete_files and log_delete_files.
  // It is not necessary to hold the mutex when invoking this method.
  void PurgeObsoleteFiles(const JobContext& background_contet);

  ColumnFamilyHandle* DefaultColumnFamily() const override;

  const SnapshotList& snapshots() const { return snapshots_; }

  void CancelAllBackgroundWork(bool wait);

  // Find Super version and reference it. Based on options, it might return
  // the thread local cached one.
  // Call ReturnAndCleanupSuperVersion() when it is no longer needed.
  SuperVersion* GetAndRefSuperVersion(ColumnFamilyData* cfd);

  // Similar to the previous function but looks up based on a column family id.
  // nullptr will be returned if this column family no longer exists.
  // REQUIRED: this function should only be called on the write thread or if the
  // mutex is held.
  SuperVersion* GetAndRefSuperVersion(uint32_t column_family_id);

  // Same as above, should called without mutex held and not on write thread.
  SuperVersion* GetAndRefSuperVersionUnlocked(uint32_t column_family_id);

  // Un-reference the super version and return it to thread local cache if
  // needed. If it is the last reference of the super version. Clean it up
  // after un-referencing it.
  void ReturnAndCleanupSuperVersion(ColumnFamilyData* cfd, SuperVersion* sv);

  // Similar to the previous function but looks up based on a column family id.
  // nullptr will be returned if this column family no longer exists.
  // REQUIRED: this function should only be called on the write thread.
  void ReturnAndCleanupSuperVersion(uint32_t colun_family_id, SuperVersion* sv);

  // Same as above, should called without mutex held and not on write thread.
  void ReturnAndCleanupSuperVersionUnlocked(uint32_t colun_family_id,
                                            SuperVersion* sv);

  // REQUIRED: this function should only be called on the write thread or if the
  // mutex is held.  Return value only valid until next call to this function or
  // mutex is released.
  ColumnFamilyHandle* GetColumnFamilyHandle(uint32_t column_family_id);

  // Same as above, should called without mutex held and not on write thread.
  ColumnFamilyHandle* GetColumnFamilyHandleUnlocked(uint32_t column_family_id);

  // Returns the number of currently running flushes.
  // REQUIREMENT: mutex_ must be held when calling this function.
  int num_running_flushes() {
    mutex_.AssertHeld();
    return num_running_flushes_;
  }

  // Returns the number of currently running compactions.
  // REQUIREMENT: mutex_ must be held when calling this function.
  int num_running_compactions() {
    mutex_.AssertHeld();
    return num_total_running_compactions_;
  }

  int num_running_large_compactions() {
    mutex_.AssertHeld();
    return num_running_large_compactions_;
  }

  // Imports data from other database dir. Source database is left unmodified.
  // Checks that source database has appropriate seqno.
  // I.e. seqno ranges of imported database does not overlap with seqno ranges of destination db.
  // And max seqno of imported database is less that active seqno of destination db.
  Status Import(const std::string& source_dir) override;

  bool AreWritesStopped();
  bool NeedsDelay() override;

  Result<std::string> GetMiddleKey() override;

  void SetAllowCompactionFailures(AllowCompactionFailures allow_compaction_failures) override;

  // Returns a table reader for the largest SST file.
  Result<TableReader*> TEST_GetLargestSstTableReader() override;

  // Used in testing to make the old memtable immutable and start writing to a new one.
  void TEST_SwitchMemtable() override;

 protected:
  Env* const env_;
  Env* const checkpoint_env_;
  const std::string dbname_;
  std::unique_ptr<VersionSet> versions_;
  const DBOptions db_options_;
  std::shared_ptr<Statistics> stats_;
  InternalIterator* NewInternalIterator(const ReadOptions&,
                                        ColumnFamilyData* cfd,
                                        SuperVersion* super_version,
                                        Arena* arena);

  // TODO(index_iter): consider using arena.
  template <typename IndexInternalIteratorType, bool kSkipLastEntry>
  std::unique_ptr<MergingIterator<IndexInternalIteratorType>> NewMergedIndexInternalIterator(
      const ReadOptions&, ColumnFamilyData* cfd, SuperVersion* super_version);

  template <typename IndexIteratorType, typename IndexInternalIteratorType>
  std::unique_ptr<typename IndexIteratorType::Base> DoNewIndexIterator(
      const ReadOptions& read_options, SkipLastEntry skip_last_index_entry,
      ColumnFamilyHandle* column_family);

  // Except in DB::Open(), WriteOptionsFile can only be called when:
  // 1. WriteThread::Writer::EnterUnbatched() is used.
  // 2. db_mutex is held
  Status WriteOptionsFile();

  // The following two functions can only be called when:
  // 1. WriteThread::Writer::EnterUnbatched() is used.
  // 2. db_mutex is NOT held
  Status RenameTempFileToOptionsFile(const std::string& file_name);
  Status DeleteObsoleteOptionsFiles();

  void NotifyOnFlushCompleted(ColumnFamilyData* cfd, FileMetaData* file_meta,
                              const MutableCFOptions& mutable_cf_options,
                              int job_id, TableProperties prop);

  void NotifyOnCompactionCompleted(ColumnFamilyData* cfd,
                                   Compaction *c, const Status &st,
                                   const CompactionJobStats& job_stats,
                                   int job_id);

  void NotifyOnNoOpCompactionCompleted(const ColumnFamilyData& cfd,
                                       const CompactionReason compaction_reason);

  Status WriteImpl(const WriteOptions& options, WriteBatch* updates,
                   WriteCallback* callback);

 private:
  friend class DB;
  friend class InternalStats;
  friend class ForwardIterator;
  friend struct SuperVersion;
  friend class CompactedDBImpl;
#ifndef NDEBUG
  friend class XFTransactionWriteHandler;
#endif
  struct CompactionState;

  struct ManualCompaction;

  struct WriteContext;

  class ThreadPoolTask;

  class CompactionTask;
  friend class CompactionTask;

  class FlushTask;
  friend class FlushTask;

  class TaskPriorityUpdater;
  friend class TaskPriorityUpdater;

  Status NewDB();

  // Recover the descriptor from persistent storage.  May do a significant
  // amount of work to recover recently logged updates.  Any changes to
  // be made to the descriptor are added to *edit.
  Status Recover(const std::vector<ColumnFamilyDescriptor>& column_families,
                 bool read_only = false, bool error_if_log_file_exist = false);

  void MaybeIgnoreError(Status* s) const;

  const Status CreateArchivalDirectory();

  // Delete any unneeded files and stale in-memory entries.
  void DeleteObsoleteFiles();

  // Flush the in-memory write buffer to storage.  Switches to a new
  // log-file/memtable and writes a new descriptor iff successful.
  Result<FileNumbersHolder> FlushMemTableToOutputFile(
      ColumnFamilyData* cfd, const MutableCFOptions& mutable_cf_options,
      bool* made_progress, JobContext* job_context, LogBuffer* log_buffer);

  // REQUIRES: log_numbers are sorted in ascending order
  Status RecoverLogFiles(const std::vector<uint64_t>& log_numbers,
                         SequenceNumber* max_sequence, bool read_only);

  // The following two methods are used to flush a memtable to
  // storage. The first one is used atdatabase RecoveryTime (when the
  // database is opened) and is heavyweight because it holds the mutex
  // for the entire period. The second method WriteLevel0Table supports
  // concurrent flush memtables to storage.
  Status WriteLevel0TableForRecovery(int job_id, ColumnFamilyData* cfd,
                                     MemTable* mem, VersionEdit* edit);

  // num_bytes: for slowdown case, delay time is calculated based on
  //            `num_bytes` going through.
  Status DelayWrite(uint64_t num_bytes);

  Status ScheduleFlushes(WriteContext* context);

  Status SwitchMemtable(ColumnFamilyData* cfd, WriteContext* context);

  // Force current memtable contents to be flushed.
  Status FlushMemTable(ColumnFamilyData* cfd, const FlushOptions& options);

  // Wait for memtable flushed
  Status WaitForFlushMemTable(ColumnFamilyData* cfd);

  Status CompactFilesImpl(
      const CompactionOptions& compact_options, ColumnFamilyData* cfd,
      Version* version, const std::vector<std::string>& input_file_names,
      const int output_level, int output_path_id, JobContext* job_context,
      LogBuffer* log_buffer);

  ColumnFamilyData* GetColumnFamilyDataByName(const std::string& cf_name);

  void MaybeScheduleFlushOrCompaction();
  void SchedulePendingFlush(ColumnFamilyData* cfd);
  void SchedulePendingCompaction(ColumnFamilyData* cfd);
  static void BGWorkCompaction(void* arg);
  static void BGWorkFlush(void* db);
  static void UnscheduleCallback(void* arg);
  void WaitAfterBackgroundError(const Status& s, const char* job_name, LogBuffer* log_buffer);
  void BackgroundCallCompaction(
      ManualCompaction* manual_compaction, CompactionTask* compaction_task = nullptr);
  void BackgroundCallFlush(ColumnFamilyData* cfd);
  Result<FileNumbersHolder> BackgroundCompaction(
      bool* made_progress, JobContext* job_context, LogBuffer* log_buffer,
      ManualCompaction* manual_compaction, CompactionTask* compaction_task);
  Result<FileNumbersHolder> BackgroundFlush(
      bool* made_progress, JobContext* job_context, LogBuffer* log_buffer, ColumnFamilyData* cfd);
  void BackgroundJobComplete(const Status& s, JobContext* job_context, LogBuffer* log_buffer);

  uint64_t GetCurrentVersionSstFilesSize() override;

  uint64_t GetCurrentVersionSstFilesUncompressedSize() override;

  std::pair<uint64_t, uint64_t> GetCurrentVersionSstFilesAllSizes() override;

  uint64_t GetCurrentVersionDataSstFilesSize() override;

  uint64_t GetCurrentVersionNumSSTFiles() override;

  int GetCfdImmNumNotFlushed() override;

  // Updates stats_ object with SST files size metrics.
  void SetSSTFileTickers();

  // Return the minimum empty level that could hold the total data in the
  // input level. Return the input level, if such level could not be found.
  int FindMinimumEmptyLevelFitting(ColumnFamilyData* cfd,
      const MutableCFOptions& mutable_cf_options, int level);

  // Move the files in the input level to the target level.
  // If target_level < 0, automatically calculate the minimum level that could
  // hold the data set.
  Status ReFitLevel(ColumnFamilyData* cfd, int level, int target_level = -1);

  // Helper functions for adding and removing from flush & compaction queues.
  void MaybeAddToCompactionQueue(ColumnFamilyData* cfd, bool use_priority_thread_pool);
  std::unique_ptr<Compaction> PopFirstFromSmallCompactionQueue();
  std::unique_ptr<Compaction> PopFirstFromLargeCompactionQueue();
  bool IsEmptyCompactionQueue();
  void AddToFlushQueue(ColumnFamilyData* cfd);
  ColumnFamilyData* PopFirstFromFlushQueue();

  // Compaction is marked as large based on options, so cannot be static or free function.
  CompactionSizeKind GetCompactionSizeKind(const Compaction& compaction);

  // helper function to call after some of the logs_ were synced
  void MarkLogsSynced(uint64_t up_to, bool synced_dir, const Status& status);

  const Snapshot* GetSnapshotImpl(bool is_write_conflict_boundary);

  Status ApplyVersionEdit(VersionEdit* edit);

  void SubmitCompactionOrFlushTask(std::unique_ptr<ThreadPoolTask> task);

  // Returns true if we have some background work.
  // I.e. scheduled but not complete compaction or flush.
  // prefix is used for logging.
  bool CheckBackgroundWorkAndLog(const char* prefix) const;

  void ListenFilesChanged(std::function<void()> listener) override;

  std::function<void()> GetFilesChangedListener() const;

  bool HasFilesChangedListener() const;

  void FilesChanged();

  bool IsShuttingDown() { return shutting_down_.load(std::memory_order_acquire); }

  struct TaskPriorityChange {
    size_t task_serial_no;
    int new_priority;
  };

  const std::string& LogPrefix() const;

  // table_cache_ provides its own synchronization
  std::shared_ptr<Cache> table_cache_;

  // Lock over the persistent DB state.  Non-nullptr iff successfully acquired.
  FileLock* db_lock_;

  // The mutex for options file related operations.
  // NOTE: should never acquire options_file_mutex_ and mutex_ at the
  //       same time.
  InstrumentedMutex options_files_mutex_;
  // State below is protected by mutex_
  InstrumentedMutex mutex_;

  std::atomic<bool> shutting_down_;

  // This condition variable is signaled when state of having background work is changed.
  InstrumentedCondVar bg_cv_;

  uint64_t logfile_number_;
  std::deque<uint64_t>
      log_recycle_files;  // a list of log files that we can recycle
  bool log_dir_synced_;
  bool log_empty_;
  ColumnFamilyHandleImpl* default_cf_handle_;
  InternalStats* default_cf_internal_stats_;
  std::unique_ptr<ColumnFamilyMemTablesImpl> column_family_memtables_;
  struct LogFileNumberSize {
    explicit LogFileNumberSize(uint64_t _number)
        : number(_number) {}
    void AddSize(uint64_t new_size) { size += new_size; }
    uint64_t number;
    uint64_t size = 0;
    bool getting_flushed = false;
  };
  struct LogWriterNumber {
    // pass ownership of _writer
    LogWriterNumber(uint64_t _number, log::Writer* _writer)
        : number(_number), writer(_writer) {}

    log::Writer* ReleaseWriter() {
      auto* w = writer;
      writer = nullptr;
      return w;
    }
    void ClearWriter() {
      delete writer;
      writer = nullptr;
    }

    uint64_t number;
    // Visual Studio doesn't support deque's member to be noncopyable because
    // of a unique_ptr as a member.
    log::Writer* writer;  // own
    // true for some prefix of logs_
    bool getting_synced = false;
  };
  std::deque<LogFileNumberSize> alive_log_files_;
  // Log files that aren't fully synced, and the current log file.
  // Synchronization:
  //  - push_back() is done from write thread with locked mutex_,
  //  - pop_front() is done from any thread with locked mutex_,
  //  - back() and items with getting_synced=true are not popped,
  //  - it follows that write thread with unlocked mutex_ can safely access
  //    back() and items with getting_synced=true.
  std::deque<LogWriterNumber> logs_;
  // Signaled when getting_synced becomes false for some of the logs_.
  InstrumentedCondVar log_sync_cv_;

  uint64_t total_log_size() {
    // TODO: use a weaker memory order for higher performance?
    const int64_t total_log_size_signed = total_log_size_.load();
    assert(total_log_size_signed >= 0);
    if (total_log_size_signed < 0)  // Just in case, for release builds.
      return 0;
    return static_cast<uint64_t>(total_log_size_signed);
  }

  // We are using a signed int for the total log size to avoid weird effects in case of underflow.
  std::atomic<int64_t> total_log_size_;
  // only used for dynamically adjusting max_total_wal_size. it is a sum of
  // [write_buffer_size * max_write_buffer_number] over all column families
  uint64_t max_total_in_memory_state_;
  // If true, we have only one (default) column family. We use this to optimize
  // some code-paths
  bool single_column_family_mode_;
  // If this is non-empty, we need to delete these log files in background
  // threads. Protected by db mutex.
  autovector<log::Writer*> logs_to_free_;

  bool is_snapshot_supported_;

  // Class to maintain directories for all database paths other than main one.
  class Directories {
   public:
    Status SetDirectories(Env* env, const std::string& dbname,
                          const std::string& wal_dir,
                          const std::vector<DbPath>& data_paths);

    Directory* GetDataDir(size_t path_id);

    Directory* GetWalDir() {
      if (wal_dir_) {
        return wal_dir_.get();
      }
      return db_dir_.get();
    }

    Directory* GetDbDir() { return db_dir_.get(); }

   private:
    std::unique_ptr<Directory> db_dir_;
    std::vector<std::unique_ptr<Directory>> data_dirs_;
    std::unique_ptr<Directory> wal_dir_;

    Status CreateAndNewDirectory(Env* env, const std::string& dirname,
                                 std::unique_ptr<Directory>* directory) const;
  };

  Directories directories_;

  WriteBuffer write_buffer_;

  WriteThread write_thread_;

#ifndef NDEBUG
  std::atomic<int> write_waiters_{0};
#endif

  WriteBatch tmp_batch_;

  WriteController write_controller_;

  // Size of the last batch group. In slowdown mode, next write needs to
  // sleep if it uses up the quota.
  uint64_t last_batch_group_size_;

  FlushScheduler flush_scheduler_;

  SnapshotList snapshots_;

  // For each background job, pending_outputs_ keeps the file number being written by that job.
  // FindObsoleteFiles()/PurgeObsoleteFiles() never deletes any file that is present in
  // pending_outputs_. After a background job is done executing, its file number is
  // deleted from pending_outputs_, which allows PurgeObsoleteFiles() to clean
  // it up.
  //
  // Background job needs to call
  //   {
  //     auto file_num_holder = pending_outputs_->NewFileNumber();
  //     auto file_num = *file_num_holder;
  //     <do something>
  //   }
  // This will protect file with number `file_num` from being deleted while <do something> is
  // running. NewFileNumber() will allocate new file number and append it to pending_outputs_.
  // This will prevent any background process to delete this file. File number will be
  // automatically removed from pending_outputs_ when file_num_holder is released on exit outside
  // of the scope.
  std::unique_ptr<FileNumbersProvider> pending_outputs_;

  // flush_queue_ and compaction_queue_ hold column families that we need to
  // flush and compact, respectively.
  // A column family is inserted into flush_queue_ when it satisfies condition
  // cfd->imm()->IsFlushPending()
  // A column family is inserted into compaction_queue_ when it satisfied
  // condition cfd->NeedsCompaction()
  // Column families in this list are all Ref()-erenced
  // TODO(icanadi) Provide some kind of ReferencedColumnFamily class that will
  // do RAII on ColumnFamilyData
  // Column families are in this queue when they need to be flushed or
  // compacted. Consumers of these queues are flush and compaction threads. When
  // column family is put on this queue, we increase unscheduled_flushes_ and
  // unscheduled_compactions_. When these variables are bigger than zero, that
  // means we need to schedule background threads for compaction and thread.
  // Once the background threads are scheduled, we decrease unscheduled_flushes_
  // and unscheduled_compactions_. That way we keep track of number of
  // compaction and flush threads we need to schedule. This scheduling is done
  // in MaybeScheduleFlushOrCompaction()
  // invariant(column family present in flush_queue_ <==>
  // ColumnFamilyData::pending_flush_ == true)
  std::deque<ColumnFamilyData*> flush_queue_;
  // invariant(number of column families in compaction_queue_ ==
  // ColumnFamilyData::num_pending_compactions_)
  std::deque<std::unique_ptr<Compaction>> small_compaction_queue_;
  std::deque<std::unique_ptr<Compaction>> large_compaction_queue_;
  int unscheduled_flushes_;
  int unscheduled_compactions_;

  // count how many background compactions are running or have been scheduled
  // This variable is left untouched when priority thread pool is used.
  int bg_compaction_scheduled_;

  // Those tasks are managed by thread pool.
  // And we remove them from this set, when they are processed/aborted by thread pool.
  std::unordered_set<CompactionTask*> compaction_tasks_;

  // stores the total number of compactions that are currently running
  int num_total_running_compactions_;

  // stores the number of large compaction that are currently running
  int num_running_large_compactions_;

  // number of background memtable flush jobs, submitted to the HIGH pool
  int bg_flush_scheduled_;

  // stores the number of flushes are currently running
  int num_running_flushes_;

  // Tracks state changes for priority thread pool tasks.
  // Metrics are updated within the PriorityThreadPoolTask.
  std::shared_ptr<RocksDBPriorityThreadPoolMetrics> priority_thread_pool_metrics_;

  // Information for a manual compaction
  struct ManualCompaction {
    ColumnFamilyData* cfd;
    int input_level;
    int output_level;
    Status status;
    bool done;
    bool in_progress;             // compaction request being processed?
    bool incomplete;              // only part of requested range compacted
    bool exclusive;               // current behavior of only one manual
    bool disallow_trivial_move;   // Force actual compaction to run
    bool has_input_size_limit;    // if true, consider the mark incompete after it's actually done
    const InternalKey* begin;     // nullptr means beginning of key range
    const InternalKey* end;       // nullptr means end of key range
    InternalKey* manual_end;      // how far we are compacting
    InternalKey tmp_storage;      // Used to keep track of compaction progress
    InternalKey tmp_storage1;     // Used to keep track of compaction progress
    std::unique_ptr<Compaction> compaction;
  };
  std::deque<ManualCompaction*> manual_compaction_dequeue_;

  struct CompactionArg {
    DBImpl* db;
    ManualCompaction* m;
  };

  // Have we encountered a background error in paranoid mode?
  Status bg_error_;

  // shall we disable deletion of obsolete files
  // if 0 the deletion is enabled.
  // if non-zero, files will not be getting deleted
  // This enables two different threads to call
  // EnableFileDeletions() and DisableFileDeletions()
  // without any synchronization
  int disable_delete_obsolete_files_;

  // next time when we should run DeleteObsoleteFiles with full scan
  uint64_t delete_obsolete_files_next_run_;

  // last time stats were dumped to LOG
  std::atomic<uint64_t> last_stats_dump_time_microsec_;

  // Each flush or compaction gets its own job id. this counter makes sure
  // they're unique
  std::atomic<int> next_job_id_;

  // A flag indicating whether the current rocksdb database has any
  // data that is not yet persisted into either WAL or SST file.
  // Used when disableWAL is true.
  bool has_unpersisted_data_;

  static const int KEEP_LOG_FILE_NUM = 1000;
  // MSVC version 1800 still does not have constexpr for ::max()
  static const uint64_t kNoTimeOut = port::kMaxUint64;

  std::string db_absolute_path_;

  // The options to access storage files
  const EnvOptions env_options_;

  WalManager wal_manager_;

  // Unified interface for logging events
  EventLogger event_logger_;

  // A value of > 0 temporarily disables scheduling of background work
  int bg_work_paused_;

  // A value of > 0 temporarily disables scheduling of background compaction
  int bg_compaction_paused_;

  // Guard against multiple concurrent refitting
  bool refitting_level_;

  // Indicate DB was opened successfully
  bool opened_successfully_;

  // Returns flush tick of the last flush of this DB.
  int64_t last_flush_at_tick_ = 0;

  // Whether DB should be flushed on shutdown.
  std::atomic<bool> disable_flush_on_shutdown_{false};

  mutable std::mutex files_changed_listener_mutex_;

  std::function<void()> files_changed_listener_ GUARDED_BY(files_changed_listener_mutex_);

  std::atomic<bool> allow_compaction_failures_{false};

  // No copying allowed
  DBImpl(const DBImpl&) = delete;
  void operator=(const DBImpl&) = delete;

  // Background threads call this function, which is just a wrapper around
  // the InstallSuperVersion() function. Background threads carry
  // job_context which can have new_superversion already
  // allocated.
  void InstallSuperVersionAndScheduleWorkWrapper(
      ColumnFamilyData* cfd, JobContext* job_context,
      const MutableCFOptions& mutable_cf_options);

  // All ColumnFamily state changes go through this function. Here we analyze
  // the new state and we schedule background work if we detect that the new
  // state needs flush or compaction.
  std::unique_ptr<SuperVersion> InstallSuperVersionAndScheduleWork(
      ColumnFamilyData* cfd, SuperVersion* new_sv,
      const MutableCFOptions& mutable_cf_options);

  using DB::GetPropertiesOfAllTables;
  virtual Status GetPropertiesOfAllTables(ColumnFamilyHandle* column_family,
                                          TablePropertiesCollection* props)
      override;
  virtual Status GetPropertiesOfTablesInRange(
      ColumnFamilyHandle* column_family, const Range* range, std::size_t n,
      TablePropertiesCollection* props) override;

  // Obtains all column family options and corresponding names,
  // dropped columns are not included into the resulting collections.
  // REQUIREMENT: mutex_ must be held when calling this function.
  void GetColumnFamiliesOptionsUnlocked(
      std::vector<std::string>* column_family_names,
      std::vector<ColumnFamilyOptions>* column_family_options);

  // Function that Get and KeyMayExist call with no_io true or false
  // Note: 'value_found' from KeyMayExist propagates here
  Status GetImpl(const ReadOptions& options, ColumnFamilyHandle* column_family,
                 const Slice& key, std::string* value,
                 bool* value_found = nullptr);

  bool GetIntPropertyInternal(ColumnFamilyData* cfd,
                              const DBPropertyInfo& property_info,
                              bool is_locked, uint64_t* value);

  bool HasPendingManualCompaction();
  bool HasExclusiveManualCompaction();
  void AddManualCompaction(ManualCompaction* m);
  void RemoveManualCompaction(ManualCompaction* m);
  bool ShouldntRunManualCompaction(ManualCompaction* m);
  bool HaveManualCompaction(ColumnFamilyData* cfd);
  bool MCOverlap(ManualCompaction* m, ManualCompaction* m1);
};

// Sanitize db options.  The caller should delete result.info_log if
// it is not equal to src.info_log.
extern Options SanitizeOptions(const std::string& db,
                               const InternalKeyComparator* icmp,
                               const Options& src);
extern DBOptions SanitizeOptions(const std::string& db, const DBOptions& src);

// Fix user-supplied options to be reasonable
template <class T, class V>
static void ClipToRange(T* ptr, V minvalue, V maxvalue) {
  if (static_cast<V>(*ptr) > maxvalue) *ptr = maxvalue;
  if (static_cast<V>(*ptr) < minvalue) *ptr = minvalue;
}

}  // namespace rocksdb
