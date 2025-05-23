/* Copyright (c) 2004, 2013, Oracle and/or its affiliates.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/*
  Please read ha_exmple.cc before reading this file.
  Please keep in mind that the federated storage engine implements all methods
  that are required to be implemented. handler.h has a full list of methods
  that you can implement.
*/

#include <mysql.h>

/* 
  handler::print_error has a case statement for error numbers.
  This value is (10000) is far out of range and will envoke the 
  default: case.  
  (Current error range is 120-159 from include/my_base.h)
*/
#define HA_FEDERATED_ERROR_WITH_REMOTE_SYSTEM 10000

#define FEDERATED_QUERY_BUFFER_SIZE (STRING_BUFFER_USUAL_SIZE * 5)
#define FEDERATED_RECORDS_IN_RANGE 2
#define FEDERATED_MAX_KEY_LENGTH 3500 // Same as innodb

/*
  FEDERATED_SHARE is a structure that will be shared amoung all open handlers
  The example implements the minimum of what you will probably need.
*/
typedef struct st_federated_share {
  MEM_ROOT mem_root;

  bool parsed;
  /* this key is unique db/tablename */
  const char *share_key;
  /*
    the primary select query to be used in rnd_init
  */
  char *select_query;
  /*
    remote host info, parse_url supplies
  */
  char *server_name;
  char *connection_string;
  char *scheme;
  char *connect_string;
  char *hostname;
  char *username;
  char *password;
  char *database;
  char *table_name;
  char *table;
  char *socket;
  char *sport;
  int share_key_length;
  ushort port;

  size_t table_name_length, server_name_length, connect_string_length, use_count;
  mysql_mutex_t mutex;
  THR_LOCK lock;
} FEDERATED_SHARE;

/*
  Class definition for the storage engine
*/
class ha_federated: public handler
{
  THR_LOCK_DATA lock;      /* MySQL lock */
  FEDERATED_SHARE *share;    /* Shared lock info */
  MYSQL *mysql; /* MySQL connection */
  MYSQL_RES *stored_result;
  /**
    Array of all stored results we get during a query execution.
  */
  DYNAMIC_ARRAY results;
  bool position_called, table_will_be_deleted;
  MYSQL_ROW_OFFSET current_position;  // Current position used by ::position()
  int remote_error_number;
  char remote_error_buf[FEDERATED_QUERY_BUFFER_SIZE];
  bool ignore_duplicates, replace_duplicates;
  bool insert_dup_update;
  DYNAMIC_STRING bulk_insert;

private:
  /*
      return 0 on success
      return errorcode otherwise
  */
  uint convert_row_to_internal_format(uchar *buf, MYSQL_ROW row,
                                      MYSQL_RES *result);
  bool create_where_from_key(String *to, KEY *key_info, 
                             const key_range *start_key,
                             const key_range *end_key,
                             bool records_in_range, bool eq_range);
  int stash_remote_error();

  bool append_stmt_insert(String *query);

  int read_next(uchar *buf, MYSQL_RES *result);
  int index_read_idx_with_result_set(uchar *buf, uint index,
                                     const uchar *key,
                                     uint key_len,
                                     ha_rkey_function find_flag,
                                     MYSQL_RES **result);
  int real_query(const char *query, size_t length);
  int real_connect();
public:
  ha_federated(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_federated() = default;
  /*
    Next pointer used in transaction
  */
  ha_federated *trx_next;
  /*
    The name of the index type that will be used for display
    don't implement this method unless you really have indexes
   */
  // perhaps get index type
  const char *index_type(uint inx) override { return "REMOTE"; }
  /*
    This is a list of flags that says what the storage engine
    implements. The current table flags are documented in
    handler.h
  */
  ulonglong table_flags() const override
  {
    /* fix server to be able to get remote server table flags */
    return (HA_PRIMARY_KEY_IN_READ_INDEX | HA_FILE_BASED
            | HA_REC_NOT_IN_SEQ | HA_AUTO_PART_KEY | HA_CAN_INDEX_BLOBS |
            HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE |
            HA_NO_PREFIX_CHAR_KEYS | HA_PRIMARY_KEY_REQUIRED_FOR_DELETE |
            HA_NO_TRANSACTIONS /* until fixed by WL#2952 */ |
            HA_PARTIAL_COLUMN_READ | HA_NULL_IN_KEY |
            HA_CAN_ONLINE_BACKUPS | HA_NON_COMPARABLE_ROWID |
            HA_CAN_REPAIR);
  }
  /*
    This is a bitmap of flags that says how the storage engine
    implements indexes. The current index flags are documented in
    handler.h. If you do not implement indexes, just return zero
    here.

    part is the key part to check. First key part is 0
    If all_parts it's set, MySQL want to know the flags for the combined
    index up to and including 'part'.
  */
    /* fix server to be able to get remote server index flags */
  ulong index_flags(uint inx, uint part, bool all_parts) const override
  {
    return (HA_READ_NEXT | HA_READ_RANGE | HA_READ_AFTER_KEY);
  }
  uint max_supported_record_length() const override { return HA_MAX_REC_LENGTH; }
  uint max_supported_keys() const       override { return MAX_KEY; }
  uint max_supported_key_parts() const  override { return MAX_REF_PARTS; }
  uint max_supported_key_length() const override { return FEDERATED_MAX_KEY_LENGTH; }
  uint max_supported_key_part_length() const override { return FEDERATED_MAX_KEY_LENGTH; }
  /*
    Called in test_quick_select to determine if indexes should be used.
    Normally, we need to know number of blocks . For federated we need to
    know number of blocks on remote side, and number of packets and blocks
    on the network side (?)
    Talk to Kostja about this - how to get the
    number of rows * ...
    disk scan time on other side (block size, size of the row) + network time ...
    The reason for "records * 1000" is that such a large number forces 
    this to use indexes "
  */

  IO_AND_CPU_COST scan_time() override
  {
    DBUG_PRINT("info", ("records %lu", (ulong) stats.records));
    return
    {
      0,
        (double) (stats.mean_rec_length * stats.records)/8192 * DISK_READ_COST+
        1000,
    };
  }
  IO_AND_CPU_COST keyread_time(uint index, ulong ranges, ha_rows rows,
                               ulonglong blocks) override
  {
    return {0, (double) (ranges + rows) * DISK_READ_COST };
  }
  IO_AND_CPU_COST rnd_pos_time(ha_rows rows) override
  {
    return {0, (double) rows * DISK_READ_COST };
  }
  const key_map *keys_to_use_for_scanning() override { return &key_map_full; }
  /*
    Everything below are methods that we implement in ha_federated.cc.

    Most of these methods are not obligatory, skip them and
    MySQL will treat them as not implemented
  */
  int open(const char *name, int mode, uint test_if_locked) override;    // required
  int close(void) override;                                              // required

  void start_bulk_insert(ha_rows rows, uint flags) override;
  int end_bulk_insert() override;
  int write_row(const uchar *buf) override;
  int update_row(const uchar *old_data, const uchar *new_data) override;
  int delete_row(const uchar *buf) override;
  int index_init(uint keynr, bool sorted) override;
  ha_rows estimate_rows_upper_bound() override;
  int index_read(uchar *buf, const uchar *key,
                 uint key_len, enum ha_rkey_function find_flag) override;
  int index_read_idx(uchar *buf, uint idx, const uchar *key,
                     uint key_len, enum ha_rkey_function find_flag);
  int index_next(uchar *buf) override;
  int index_end() override;
  int read_range_first(const key_range *start_key,
                               const key_range *end_key,
                               bool eq_range, bool sorted) override;
  int read_range_next() override;
  /*
    unlike index_init(), rnd_init() can be called two times
    without rnd_end() in between (it only makes sense if scan=1).
    then the second call should prepare for the new table scan
    (e.g if rnd_init allocates the cursor, second call should
    position it to the start of the table, no need to deallocate
    and allocate it again
  */
  int rnd_init(bool scan) override;                                      //required
  int rnd_end() override;
  int rnd_next(uchar *buf) override;                                      //required
  int rnd_next_int(uchar *buf);
  int rnd_pos(uchar *buf, uchar *pos) override;                            //required
  void position(const uchar *record) override;                            //required
  /*
    A ref is a pointer inside a local buffer. It is not comparable to
    other ref's.
  */
  int cmp_ref(const uchar *ref1, const uchar *ref2) override
  {
    return handler::cmp_ref(ref1,ref2);    /* Works if table scan is used */
  }
  int info(uint) override;                                              //required
  int extra(ha_extra_function operation) override;

  void update_auto_increment(void);
  int repair(THD* thd, HA_CHECK_OPT* check_opt) override;
  int optimize(THD* thd, HA_CHECK_OPT* check_opt) override;
  int delete_table(const char *name) override
  {
    return 0;
  }
  int delete_all_rows(void) override;
  int truncate() override;
  int create(const char *name, TABLE *form,
             HA_CREATE_INFO *create_info) override;                      //required
  ha_rows records_in_range(uint inx,
                           const key_range *start_key,
                           const key_range *end_key,
                           page_range *pages) override;
  uint8 table_cache_type() override { return HA_CACHE_TBL_NOCACHE; }

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override;     //required
  bool get_error_message(int error, String *buf) override;
  
  MYSQL_RES *store_result(MYSQL *mysql);
  void free_result();
  
  int external_lock(THD *thd, int lock_type) override;
  int connection_commit();
  int connection_rollback();
  int connection_autocommit(bool state);
  int execute_simple_query(const char *query, int len);
  int reset(void) override;
};
