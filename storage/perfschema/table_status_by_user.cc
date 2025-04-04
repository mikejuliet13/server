/* Copyright (c) 2015, 2023, Oracle and/or its affiliates.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file storage/perfschema/table_status_by_user.cc
  Table STATUS_BY_USER (implementation).
*/

#include "my_global.h"
#include "table_status_by_user.h"
#include "my_thread.h"
#include "pfs_instr_class.h"
#include "pfs_column_types.h"
#include "pfs_column_values.h"
#include "pfs_global.h"
#include "pfs_account.h"
#include "pfs_visitor.h"

THR_LOCK table_status_by_user::m_table_lock;

PFS_engine_table_share_state
table_status_by_user::m_share_state = {
  false /* m_checked */
};

PFS_engine_table_share
table_status_by_user::m_share=
{
  { C_STRING_WITH_LEN("status_by_user") },
  &pfs_truncatable_acl,
  table_status_by_user::create,
  NULL, /* write_row */
  table_status_by_user::delete_all_rows,
  table_status_by_user::get_row_count,
  sizeof(pos_t),
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE status_by_user("
  "USER CHAR(32) collate utf8_bin default null comment 'User for which the status variable is reported.',"
  "VARIABLE_NAME VARCHAR(64) not null comment 'Status variable name.',"
  "VARIABLE_VALUE VARCHAR(1024) comment 'Aggregated status variable value.' )") },
  false, /* m_perpetual */
  false, /* m_optional */
  &m_share_state
};

PFS_engine_table*
table_status_by_user::create(void)
{
  return new table_status_by_user();
}

int table_status_by_user::delete_all_rows(void)
{
  mysql_mutex_lock(&LOCK_status);
  reset_status_by_thread();
  reset_status_by_account();
  reset_status_by_user();
  mysql_mutex_unlock(&LOCK_status);
  return 0;
}

ha_rows table_status_by_user::get_row_count(void)
{
  mysql_mutex_lock(&LOCK_status);
  size_t status_var_count= all_status_vars.elements;
  mysql_mutex_unlock(&LOCK_status);
  return (global_user_container.get_row_count() * status_var_count);
}

table_status_by_user::table_status_by_user()
  : PFS_engine_table(&m_share, &m_pos),
    m_status_cache(true), m_row_exists(false), m_pos(), m_next_pos()
{}

void table_status_by_user::reset_position(void)
{
  m_pos.reset();
  m_next_pos.reset();
}

int table_status_by_user::rnd_init(bool scan)
{
  if (show_compatibility_56)
    return 0;

  /*
    Build array of SHOW_VARs from the global status array prior to materializing
    threads in rnd_next() or rnd_pos().
  */
  m_status_cache.initialize_client_session();

  /* Use the current number of status variables to detect changes. */
  ulonglong status_version= m_status_cache.get_status_array_version();

  /*
    The table context holds the current version of the global status array
    and a record of which users were materialized. If scan == true, then
    allocate a new context from mem_root and store in TLS. If scan == false,
    then restore from TLS.
  */
  m_context= current_thd->alloc<table_status_by_user_context>(1);
  new (m_context) table_status_by_user_context(status_version, !scan);
  return 0;
}

int table_status_by_user::rnd_next(void)
{
  if (show_compatibility_56)
    return HA_ERR_END_OF_FILE;

  /* If status array changes, exit with warning. */ // TODO: Issue warning
  if (!m_context->versions_match())
    return HA_ERR_END_OF_FILE;

  /*
    For each user, build a cache of status variables using totals from all
    threads associated with the user.
  */
  bool has_more_user= true;

  for (m_pos.set_at(&m_next_pos);
       has_more_user;
       m_pos.next_user())
  {
    PFS_user *pfs_user= global_user_container.get(m_pos.m_index_1, &has_more_user);

    if (m_status_cache.materialize_user(pfs_user) == 0)
    {
      /* Mark this user as materialized. */
      m_context->set_item(m_pos.m_index_1);

      /* Get the next status variable. */
      const Status_variable *stat_var= m_status_cache.get(m_pos.m_index_2);
      if (stat_var != NULL)
      {
        make_row(pfs_user, stat_var);
        m_next_pos.set_after(&m_pos);
        return 0;
      }
    }
  }
  return HA_ERR_END_OF_FILE;
}

int
table_status_by_user::rnd_pos(const void *pos)
{
  if (show_compatibility_56)
    return HA_ERR_RECORD_DELETED;

  /* If status array changes, exit with warning. */ // TODO: Issue warning
  if (!m_context->versions_match())
    return HA_ERR_END_OF_FILE;

  set_position(pos);
  assert(m_pos.m_index_1 < global_user_container.get_row_count());

  PFS_user *pfs_user= global_user_container.get(m_pos.m_index_1);

  /*
    Only materialize threads that were previously materialized by rnd_next().
    If a user cannot be rematerialized, then do nothing.
  */
  if (m_context->is_item_set(m_pos.m_index_1) &&
      m_status_cache.materialize_user(pfs_user) == 0)
  {
    const Status_variable *stat_var= m_status_cache.get(m_pos.m_index_2);
    if (stat_var != NULL)
    {
      make_row(pfs_user, stat_var);
      return 0;
    }
  }
  return HA_ERR_RECORD_DELETED;
}

void table_status_by_user
::make_row(PFS_user *user, const Status_variable *status_var)
{
  pfs_optimistic_state lock;
  m_row_exists= false;
  user->m_lock.begin_optimistic_lock(&lock);

  if (m_row.m_user.make_row(user))
    return;

  m_row.m_variable_name.make_row(status_var->m_name, status_var->m_name_length);
  m_row.m_variable_value.make_row(status_var);

  if (!user->m_lock.end_optimistic_lock(&lock))
    return;

  m_row_exists= true;
}

int table_status_by_user
::read_row_values(TABLE *table,
                  unsigned char *buf,
                  Field **fields,
                  bool read_all)
{
  Field *f;

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  /* Set the null bits */
  assert(table->s->null_bytes == 1);
  buf[0]= 0;

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /* USER */
        m_row.m_user.set_field(f);
        break;
      case 1: /* VARIABLE_NAME */
        set_field_varchar_utf8(f, m_row.m_variable_name.m_str, m_row.m_variable_name.m_length);
        break;
      case 2: /* VARIABLE_VALUE */
        m_row.m_variable_value.set_field(f);
        break;
      default:
        assert(false);
      }
    }
  }

  return 0;
}

