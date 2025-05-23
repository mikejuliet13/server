/* Copyright (c) 2000, 2011, Oracle and/or its affiliates
   Copyright (c) 2009, 2016, MariaDB

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
  MyISAM MERGE tables

  A MyISAM MERGE table is kind of a union of zero or more MyISAM tables.

  Besides the normal form file (.frm) a MERGE table has a meta file
  (.MRG) with a list of tables. These are paths to the MyISAM table
  files. The last two components of the path contain the database name
  and the table name respectively.

  When a MERGE table is open, there exists an TABLE object for the MERGE
  table itself and a TABLE object for each of the MyISAM tables. For
  abbreviated writing, I call the MERGE table object "parent" and the
  MyISAM table objects "children".

  A MERGE table is almost always opened through open_and_lock_tables()
  and hence through open_tables(). When the parent appears in the list
  of tables to open, the initial open of the handler does nothing but
  read the meta file and collect a list of TABLE_LIST objects for the
  children. This list is attached to the handler object as
  ha_myisammrg::children_l. The end of the children list is saved in
  ha_myisammrg::children_last_l.

  Back in open_tables(), handler::extra(HA_EXTRA_ADD_CHILDREN_LIST) is
  called. It updates each list member with the lock type and a back
  pointer to the parent TABLE_LIST object TABLE_LIST::parent_l. The list
  is then inserted in the list of tables to open, right behind the
  parent. Consequently, open_tables() opens the children, one after the
  other. The TABLE references of the TABLE_LIST objects are implicitly
  set to the open tables by open_table(). The children are opened as
  independent MyISAM tables, right as if they are used by the SQL
  statement.

  When all tables from the statement query list are open,
  handler::extra(HA_EXTRA_ATTACH_CHILDREN) is called. It "attaches" the
  children to the parent. All required references between parent and
  children are set up.

  The MERGE storage engine sets up an array with references to the
  low-level MyISAM table objects (MI_INFO). It remembers the state of
  the table in MYRG_INFO::children_attached.

  If necessary, the compatibility of parent and children is checked.
  This check is necessary when any of the objects are reopened. This is
  detected by comparing the current table def version against the
  remembered child def version. On parent open, the list members are
  initialized to an "impossible"/"undefined" version value. So the check
  is always executed on the first attach.

  The version check is done in myisammrg_attach_children_callback(),
  which is called for every child. ha_myisammrg::attach_children()
  initializes 'need_compat_check' to FALSE and
  myisammrg_attach_children_callback() sets it ot TRUE if a table
  def version mismatches the remembered child def version.

  The children chain remains in the statement query list until the table
  is closed or the children are detached. This is done so that the
  children are locked by lock_tables().

  At statement end the children are detached. At the next statement
  begin the open-add-attach sequence repeats. There is no exception for
  LOCK TABLES. The fresh establishment of the parent-child relationship
  before every statement catches numerous cases of ALTER/FLUSH/DROP/etc
  of parent or children during LOCK TABLES.

  ---

  On parent open the storage engine structures are allocated and initialized.
  They stay with the open table until its final close.
*/

#define MYSQL_SERVER 1
#include <my_global.h>
#include "sql_priv.h"
#include "unireg.h"
#include "sql_cache.h"                          // query_cache_*
#include "sql_show.h"                           // append_identifier
#include "sql_table.h"                         // build_table_filename
#include <m_ctype.h>
#include "../myisam/ha_myisam.h"
#include "ha_myisammrg.h"
#include "myrg_def.h"
#include "thr_malloc.h"                         // init_sql_alloc
#include "sql_class.h"                          // THD
#include "debug_sync.h"

static handler *myisammrg_create_handler(handlerton *hton,
                                         TABLE_SHARE *table,
                                         MEM_ROOT *mem_root)
{
  return new (mem_root) ha_myisammrg(hton, table);
}


/**
  @brief Constructor
*/

ha_myisammrg::ha_myisammrg(handlerton *hton, TABLE_SHARE *table_arg)
  :handler(hton, table_arg), file(0), is_cloned(0)
{
  init_sql_alloc(rg_key_memory_children, &children_mem_root,
                 FN_REFLEN, 0, MYF(0));
}


/**
  @brief Destructor
*/

ha_myisammrg::~ha_myisammrg(void)
{
  free_root(&children_mem_root, MYF(0));
}


static const char *ha_myisammrg_exts[] = {
  MYRG_NAME_EXT,
  NullS
};
extern int table2myisam(TABLE *table_arg, MI_KEYDEF **keydef_out,
                        MI_COLUMNDEF **recinfo_out, uint *records_out);
extern int check_definition(MI_KEYDEF *t1_keyinfo,
                            MI_COLUMNDEF *t1_recinfo,
                            uint t1_keys, uint t1_recs,
                            MI_KEYDEF *t2_keyinfo,
                            MI_COLUMNDEF *t2_recinfo,
                            uint t2_keys, uint t2_recs, bool strict,
                            TABLE *table_arg);
static void split_file_name(const char *file_name,
			    LEX_STRING *db, LEX_STRING *name);


extern "C" void myrg_print_wrong_table(const char *table_name)
{
  LEX_STRING db= {NULL, 0}, name;
  char buf[FN_REFLEN];
  split_file_name(table_name, &db, &name);
  memcpy(buf, db.str, db.length);
  buf[db.length]= '.';
  memcpy(buf + db.length + 1, name.str, name.length);
  buf[db.length + name.length + 1]= 0;
  /*
    Push an error to be reported as part of CHECK/REPAIR result-set.
    Note that calling my_error() from handler is a hack which is kept
    here to avoid refactoring. Normally engines should report errors
    through return value which will be interpreted by caller using
    handler::print_error() call.
  */
  my_error(ER_ADMIN_WRONG_MRG_TABLE, MYF(0), buf);
}


/**
  Callback function for open of a MERGE parent table.

  @param[in]    callback_param  data pointer as given to myrg_parent_open()
                                this is used to pass the handler handle
  @param[in]    filename        file name of MyISAM table
                                without extension.

  @return status
    @retval     0               OK
    @retval     != 0            Error

  @detail

    This function adds a TABLE_LIST object for a MERGE child table to a
    list of tables in the parent handler object. It is called for each
    child table.

    The list of child TABLE_LIST objects is kept in the handler object
    of the parent for the whole life time of the MERGE table. It is
    inserted in the statement query list behind the MERGE parent
    TABLE_LIST object when the MERGE table is opened. It is removed from
    the statement query list at end of statement or at children detach.

    All memory used for the child TABLE_LIST objects and the strings
    referred by it are taken from the parent
    ha_myisammrg::children_mem_root. Thus they are all freed implicitly at
    the final close of the table.

    children_l -> TABLE_LIST::next_global -> TABLE_LIST::next_global
    #             #               ^          #               ^
    #             #               |          #               |
    #             #               +--------- TABLE_LIST::prev_global
    #             #                                          |
    #       |<--- TABLE_LIST::prev_global                    |
    #                                                        |
    children_last_l -----------------------------------------+
*/

CPP_UNNAMED_NS_START

extern "C" int myisammrg_parent_open_callback(void *callback_param,
                                              const char *filename)
{
  ha_myisammrg  *ha_myrg= (ha_myisammrg*) callback_param;
  TABLE         *parent= ha_myrg->table_ptr();
  Mrg_child_def *mrg_child_def;
  LEX_STRING    db, table_name;
  char          dir_path[FN_REFLEN];
  DBUG_ENTER("myisammrg_parent_open_callback");

  /*
    Depending on MySQL version, filename may be encoded by table name to
    file name encoding or not. Always encoded if parent table is created
    by 5.1.46+. Encoded if parent is created by 5.1.6+ and child table is
    in different database.
  */
  if (!has_path(filename))
  {
    /* Child is in the same database as parent. */
    db= ha_myrg->make_child_ident(parent->s->db);
    /* Child table name is encoded in parent dot-MRG starting with 5.1.46. */
    table_name= (parent->s->mysql_version >= 50146) ?
      ha_myrg->make_child_ident_filename_to_tablename(filename,
                                                      lower_case_table_names) :
      ha_myrg->make_child_ident_opt_casedn(Lex_cstring_strlen(filename),
                                           lower_case_table_names);
  }
  else
  {
    DBUG_ASSERT(strlen(filename) < sizeof(dir_path));
    fn_format(dir_path, filename, "", "", 0);
    /* Extract child table name and database name from filename. */
    size_t dirlen= dirname_length(dir_path);
    /* Child db/table name is encoded in parent dot-MRG starting with 5.1.6. */
    if (parent->s->mysql_version >= 50106)
    {
      table_name= ha_myrg->make_child_ident_filename_to_tablename(
                                                     dir_path + dirlen,
                                                     lower_case_table_names);
      dir_path[dirlen - 1]= 0;
      dirlen= dirname_length(dir_path);
      db= ha_myrg->make_child_ident_filename_to_tablename(dir_path + dirlen,
                                                          false);
    }
    else
    {
      table_name= ha_myrg->make_child_ident_opt_casedn(
                                         Lex_cstring_strlen(dir_path + dirlen),
                                         lower_case_table_names);
      dir_path[dirlen - 1]= 0;
      dirlen= dirname_length(dir_path);
      db= ha_myrg->make_child_ident(Lex_cstring_strlen(dir_path + dirlen));
    }
  }

  if (! db.str || ! table_name.str)
    DBUG_RETURN(1);

  DBUG_PRINT("myrg", ("open: '%.*s'.'%.*s'", (int) db.length, db.str,
                      (int) table_name.length, table_name.str));

  mrg_child_def= new (&ha_myrg->children_mem_root)
                 Mrg_child_def(db.str, db.length,
                               table_name.str, table_name.length);

  if (! mrg_child_def ||
      ha_myrg->child_def_list.push_back(mrg_child_def,
                                        &ha_myrg->children_mem_root))
  {
    DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}

CPP_UNNAMED_NS_END


/*
  Set external_ref for the child MyISAM tables. They need this to be set in
  order to check for killed status.
*/
static void myrg_set_external_ref(MYRG_INFO *m_info, void *ext_ref_arg)
{
  int i;
  for (i= 0; i < (int)m_info->tables; i++)
  {
    m_info->open_tables[i].table->external_ref= ext_ref_arg;
  }
}

IO_AND_CPU_COST ha_myisammrg::rnd_pos_time(ha_rows rows)
{
  IO_AND_CPU_COST cost= handler::rnd_pos_time(rows);
  /*
    Row data is not cached. costs.row_lookup_cost includes the cost of
    the reading the row from system (probably cached by the OS).
  */
  cost.io= 0;
  return cost;
}

IO_AND_CPU_COST ha_myisammrg::keyread_time(uint index, ulong ranges,
                                           ha_rows rows,
                                           ulonglong blocks)
{
  IO_AND_CPU_COST cost= handler::keyread_time(index, ranges, rows, blocks);
  if (!blocks)
  {
    cost.io*= file->tables;
    cost.cpu*= file->tables;
  }
  /* Add the cost of having to do a key lookup in all trees */
  if (file->tables)
    cost.cpu+= (file->tables-1) * (ranges * KEY_LOOKUP_COST);
  return cost;
}

/**
  Open a MERGE parent table, but not its children.

  @param[in]    name               MERGE table path name
  @param[in]    mode               read/write mode, unused
  @param[in]    test_if_locked_arg open flags

  @return       status
  @retval     0                    OK
  @retval     -1                   Error, my_errno gives reason

  @detail
  This function initializes the MERGE storage engine structures
  and adds a child list of TABLE_LIST to the parent handler.
*/

int ha_myisammrg::open(const char *name, int mode __attribute__((unused)),
                       uint test_if_locked_arg)
{
  DBUG_ENTER("ha_myisammrg::open");
  DBUG_PRINT("myrg", ("name: '%s'  table: %p", name, table));
  DBUG_PRINT("myrg", ("test_if_locked_arg: %u", test_if_locked_arg));

  /* Must not be used when table is open. */
  DBUG_ASSERT(!this->file);

  /* Save for later use. */
  test_if_locked= test_if_locked_arg;

  /* In case this handler was open and closed before, free old data. */
  free_root(&this->children_mem_root, MYF(MY_MARK_BLOCKS_FREE));

  /*
    Initialize variables that are used, modified, and/or set by
    myisammrg_parent_open_callback().
    'children_l' is the head of the children chain.
    'children_last_l' points to the end of the children chain.
    'my_errno' is set by myisammrg_parent_open_callback() in
    case of an error.
  */
  children_l= NULL;
  children_last_l= NULL;
  child_def_list.empty();
  my_errno= 0;

  /* retrieve children table list. */
  if (is_cloned)
  {
    /*
      Open and attaches the MyISAM tables,that are under the MERGE table 
      parent, on the MyISAM storage engine interface directly within the
      MERGE engine. The new MyISAM table instances, as well as the MERGE 
      clone itself, are not visible in the table cache. This is not a 
      problem because all locking is handled by the original MERGE table
      from which this is cloned of.
    */
    if (!(file= myrg_open(name, table->db_stat,  HA_OPEN_IGNORE_IF_LOCKED)))
    {
      DBUG_PRINT("error", ("my_errno %d", my_errno));
      DBUG_RETURN(my_errno ? my_errno : -1); 
    }

    file->children_attached= TRUE;
    myrg_set_external_ref(file, (void*)table);

    info(HA_STATUS_NO_LOCK | HA_STATUS_VARIABLE | HA_STATUS_CONST);
  }
  else if (!(file= myrg_parent_open(name, myisammrg_parent_open_callback, this)))
  {
    /* purecov: begin inspected */
    DBUG_PRINT("error", ("my_errno %d", my_errno));
    DBUG_RETURN(my_errno ? my_errno : -1);
    /* purecov: end */
  }
  DBUG_PRINT("myrg", ("MYRG_INFO: %p  child tables: %u",
                      file, file->tables));
  DBUG_RETURN(0);
}


/**
  Add list of MERGE children to a TABLE_LIST chain.

  @return status
    @retval     0               OK
    @retval     != 0            Error

  @detail
    When a MERGE parent table has just been opened, insert the
    TABLE_LIST chain from the MERGE handler into the table list used for
    opening tables for this statement. This lets the children be opened
    too.
*/

int ha_myisammrg::add_children_list(void)
{
  TABLE_LIST  *parent_l= this->table->pos_in_table_list;
  THD  *thd= table->in_use;
  List_iterator_fast<Mrg_child_def> it(child_def_list);
  Mrg_child_def *mrg_child_def;
  DBUG_ENTER("ha_myisammrg::add_children_list");
  DBUG_PRINT("myrg", ("table: '%s'.'%s' %p", this->table->s->db.str,
                      this->table->s->table_name.str, this->table));

  /* Must call this with open table. */
  DBUG_ASSERT(this->file);

  /* Ignore this for empty MERGE tables (UNION=()). */
  if (!this->file->tables)
  {
    DBUG_PRINT("myrg", ("empty merge table union"));
    goto end;
  }

  /* Must not call this with attached children. */
  DBUG_ASSERT(!this->file->children_attached);

  /* Must not call this with children list in place. */
  DBUG_ASSERT(this->children_l == NULL);

  /*
    Prevent inclusion of another MERGE table, which could make infinite
    recursion.
  */
  if (parent_l->parent_l)
  {
    my_error(ER_ADMIN_WRONG_MRG_TABLE, MYF(0), parent_l->alias.str);
    DBUG_RETURN(1);
  }

  while ((mrg_child_def= it++))
  {
    TABLE_LIST  *child_l;
    LEX_CSTRING db;
    LEX_CSTRING table_name;

    child_l= thd->alloc<TABLE_LIST>(1);
    db.str= (char*) thd->memdup(mrg_child_def->db.str, mrg_child_def->db.length+1);
    db.length= mrg_child_def->db.length;
    table_name.str= (char*) thd->memdup(mrg_child_def->name.str,
                                        mrg_child_def->name.length+1);
    table_name.length= mrg_child_def->name.length;

    if (child_l == NULL || db.str == NULL || table_name.str == NULL)
      DBUG_RETURN(1);

    child_l->init_one_table(&db, &table_name, 0, parent_l->lock_type);
    /* Set parent reference. Used to detect MERGE in children list. */
    child_l->parent_l= parent_l;
    /* Copy select_lex. Used in unique_table() at least. */
    child_l->select_lex= parent_l->select_lex;
    /* Set the expected table version, to not cause spurious re-prepare. */
    child_l->set_table_ref_id(mrg_child_def->get_child_table_ref_type(),
                              mrg_child_def->get_child_def_version());
    /*
      Copy parent's prelocking attribute to allow opening of child
      temporary residing in the prelocking list.
    */
    child_l->prelocking_placeholder= parent_l->prelocking_placeholder;
    /*
      For statements which acquire a SNW metadata lock on a parent table and
      then later try to upgrade it to an X lock (e.g. ALTER TABLE), SNW
      locks should be also taken on the children tables.

      Otherwise we end up in a situation where the thread trying to upgrade SNW
      to X lock on the parent also holds a SR metadata lock and a read
      thr_lock.c lock on the child. As a result, another thread might be
      blocked on the thr_lock.c lock for the child after successfully acquiring
      a SR or SW metadata lock on it. If at the same time this second thread
      has a shared metadata lock on the parent table or there is some other
      thread which has a shared metadata lock on the parent and is waiting for
      this second thread, we get a deadlock. This deadlock cannot be properly
      detected by the MDL subsystem as part of the waiting happens within
      thr_lock.c. By taking SNW locks on the child tables we ensure that any
      thread which waits for a thread doing SNW -> X upgrade, does this within
      the MDL subsystem and thus potential deadlocks are exposed to the deadlock
      detector.

      We don't do the same thing for SNRW locks as this would allow
      DDL on implicitly locked underlying tables of a MERGE table.
    */
    if (! thd->locked_tables_mode &&
        parent_l->mdl_request.type == MDL_SHARED_UPGRADABLE)
      child_l->mdl_request.set_type(MDL_SHARED_NO_WRITE);
    /* Link TABLE_LIST object into the children list. */
    if (this->children_last_l)
      child_l->prev_global= this->children_last_l;
    else
    {
      /* Initialize children_last_l when handling first child. */
      this->children_last_l= &this->children_l;
    }
    *this->children_last_l= child_l;
    this->children_last_l= &child_l->next_global;
  }

  /* Insert children into the table list. */
  if (parent_l->next_global)
    parent_l->next_global->prev_global= this->children_last_l;
  *this->children_last_l= parent_l->next_global;
  parent_l->next_global= this->children_l;
  this->children_l->prev_global= &parent_l->next_global;
  /*
    We have to update LEX::query_tables_last if children are added to
    the tail of the table list in order to be able correctly add more
    elements to it (e.g. as part of prelocking process).
  */
  if (thd->lex->query_tables_last == &parent_l->next_global)
    thd->lex->query_tables_last= this->children_last_l;
  /*
    The branch below works only when re-executing a prepared
    statement or a stored routine statement:
    We've just modified query_tables_last. Keep it in sync with
    query_tables_last_own, if it was set by the prelocking code.
    This ensures that the check that prohibits double updates (*)
    can correctly identify what tables belong to the main statement.
    (*) A double update is, e.g. when a user issues UPDATE t1 and
    t1 has an AFTER UPDATE trigger that also modifies t1.
  */
  if (thd->lex->query_tables_own_last == &parent_l->next_global)
    thd->lex->query_tables_own_last= this->children_last_l;

end:
  DBUG_RETURN(0);
}


/**
  A context of myrg_attach_children() callback.
*/

class Mrg_attach_children_callback_param
{
public:
  /**
    'need_compat_check' is set by myisammrg_attach_children_callback()
    if a child fails the table def version check.
  */
  bool need_compat_check;
  /** TABLE_LIST identifying this merge parent. */
  TABLE_LIST *parent_l;
  /** Iterator position, the current child to attach. */
  TABLE_LIST *next_child_attach;
  List_iterator_fast<Mrg_child_def> def_it;
  Mrg_child_def *mrg_child_def;
public:
  Mrg_attach_children_callback_param(TABLE_LIST *parent_l_arg,
                                     TABLE_LIST *first_child,
                                     List<Mrg_child_def> &child_def_list)
    :need_compat_check(FALSE),
    parent_l(parent_l_arg),
    next_child_attach(first_child),
    def_it(child_def_list),
    mrg_child_def(def_it++)
  {}
  void next()
  {
    next_child_attach= next_child_attach->next_global;
    if (next_child_attach && next_child_attach->parent_l != parent_l)
      next_child_attach= NULL;
    if (mrg_child_def)
      mrg_child_def= def_it++;
  }
};


/**
  Callback function for attaching a MERGE child table.

  @param[in]    callback_param  data pointer as given to myrg_attach_children()
                                this is used to pass the handler handle

  @return       pointer to open MyISAM table structure
    @retval     !=NULL                  OK, returning pointer
    @retval     NULL,                   Error.

  @detail
    This function retrieves the MyISAM table handle from the
    next child table. It is called for each child table.
*/

CPP_UNNAMED_NS_START

extern "C" MI_INFO *myisammrg_attach_children_callback(void *callback_param)
{
  Mrg_attach_children_callback_param *param=
    (Mrg_attach_children_callback_param*) callback_param;
  TABLE         *parent= param->parent_l->table;
  TABLE         *child;
  TABLE_LIST    *child_l= param->next_child_attach;
  Mrg_child_def *mrg_child_def= param->mrg_child_def;
  MI_INFO       *myisam= NULL;
  DBUG_ENTER("myisammrg_attach_children_callback");

  /*
    Number of children in the list and MYRG_INFO::tables_count,
    which is used by caller of this function, should always match.
  */
  DBUG_ASSERT(child_l);

  child= child_l->table;
  /* Prepare for next child. */
  param->next();

  /*
    When MERGE table is opened for CHECK or REPAIR TABLE statements,
    failure to open any of underlying tables is ignored until this moment
    (this is needed to provide complete list of the problematic underlying
    tables in CHECK/REPAIR TABLE output).
    Here we detect such a situation and report an appropriate error.
  */
  if (! child)
  {
    DBUG_PRINT("error", ("failed to open underlying table '%s'.'%s'",
                         child_l->db.str, child_l->table_name.str));
    /*
      This should only happen inside of CHECK/REPAIR TABLE or
      for the tables added by the pre-locking code.
    */
    DBUG_ASSERT(current_thd->open_options & HA_OPEN_FOR_REPAIR ||
                child_l->prelocking_placeholder);
    goto end;
  }

  /*
    Do a quick compatibility check. The table def version is set when
    the table share is created. The child def version is copied
    from the table def version after a successful compatibility check.
    We need to repeat the compatibility check only if a child is opened
    from a different share than last time it was used with this MERGE
    table.
  */
  DBUG_PRINT("myrg", ("table_def_version last: %lu  current: %lu",
                      (ulong) mrg_child_def->get_child_def_version(),
                      (ulong) child->s->get_table_def_version()));
  if (mrg_child_def->get_child_def_version() != child->s->get_table_def_version())
    param->need_compat_check= TRUE;

  /*
    If child is temporary, parent must be temporary as well. Other
    parent/child combinations are allowed. This check must be done for
    every child on every open because the table def version can overlap
    between temporary and non-temporary tables. We need to detect the
    case where a non-temporary table has been replaced with a temporary
    table of the same version. Or vice versa. A very unlikely case, but
    it could happen. (Note that the condition was different from
    5.1.23/6.0.4(Bug#19627) to 5.5.6 (Bug#36171): child->s->tmp_table !=
    parent->s->tmp_table. Tables were required to have the same status.)
  */
  if (child->s->tmp_table && !parent->s->tmp_table)
  {
    DBUG_PRINT("error", ("temporary table mismatch parent: %d  child: %d",
                         parent->s->tmp_table, child->s->tmp_table));
    goto end;
  }

  /* Extract the MyISAM table structure pointer from the handler object. */
  if ((child->file->ht->db_type != DB_TYPE_MYISAM) ||
      !(myisam= ((ha_myisam*) child->file)->file_ptr()))
  {
    DBUG_PRINT("error", ("no MyISAM handle for child table: '%s'.'%s' %p",
                         child->s->db.str, child->s->table_name.str,
                         child));
  }

  DBUG_PRINT("myrg", ("MyISAM handle: %p", myisam));

 end:

  if (!myisam &&
      (current_thd->open_options & HA_OPEN_FOR_REPAIR))
  {
    char buf[2*NAME_LEN + 1 + 1];
    strxnmov(buf, sizeof(buf) - 1, child_l->db.str, ".",
             child_l->table_name.str, NULL);
    /*
      Push an error to be reported as part of CHECK/REPAIR result-set.
      Note that calling my_error() from handler is a hack which is kept
      here to avoid refactoring. Normally engines should report errors
      through return value which will be interpreted by caller using
      handler::print_error() call.
    */
    my_error(ER_ADMIN_WRONG_MRG_TABLE, MYF(0), buf);
  }

  DBUG_RETURN(myisam);
}

CPP_UNNAMED_NS_END

/**
   Returns a cloned instance of the current handler.

   @return A cloned handler instance.
 */
handler *ha_myisammrg::clone(const char *name, MEM_ROOT *mem_root)
{
  MYRG_TABLE    *u_table,*newu_table;
  ha_myisammrg *new_handler= 
    (ha_myisammrg*) get_new_handler(table->s, mem_root, table->s->db_type());
  if (!new_handler)
    return NULL;
  
  /* Inform ha_myisammrg::open() that it is a cloned handler */
  new_handler->is_cloned= TRUE;
  /*
    Allocate handler->ref here because otherwise ha_open will allocate it
    on this->table->mem_root and we will not be able to reclaim that memory 
    when the clone handler object is destroyed.
  */
  if (!(new_handler->ref= (uchar*) alloc_root(mem_root, ALIGN_SIZE(ref_length)*2)))
  {
    delete new_handler;
    return NULL;
  }

  if (new_handler->ha_open(table, name, table->db_stat,
                           HA_OPEN_IGNORE_IF_LOCKED))
  {
    delete new_handler;
    return NULL;
  }
 
  /*
    Iterate through the original child tables and
    copy the state into the cloned child tables.
    We need to do this because all the child tables
    can be involved in delete.
  */
  newu_table= new_handler->file->open_tables;
  for (u_table= file->open_tables; u_table < file->end_table; u_table++)
  {
    newu_table->table->state= u_table->table->state;
    newu_table++;
  }

  return new_handler;
 }


/**
  Attach children to a MERGE table.

  @return status
    @retval     0               OK
    @retval     != 0            Error, my_errno gives reason

  @detail
    Let the storage engine attach its children through a callback
    function. Check table definitions for consistency.

  @note
    Special thd->open_options may be in effect. We can make use of
    them in attach. I.e. we use HA_OPEN_FOR_REPAIR to report the names
    of mismatching child tables. We cannot transport these options in
    ha_myisammrg::test_if_locked because they may change after the
    parent is opened. The parent is kept open in the table cache over
    multiple statements and can be used by other threads. Open options
    can change over time.
*/

int ha_myisammrg::attach_children(void)
{
  MYRG_TABLE    *u_table;
  MI_COLUMNDEF  *recinfo;
  MI_KEYDEF     *keyinfo;
  uint          recs;
  uint          keys= table->s->keys;
  TABLE_LIST   *parent_l= table->pos_in_table_list;
  int           error;
  Mrg_attach_children_callback_param param(parent_l, this->children_l, child_def_list);
  DBUG_ENTER("ha_myisammrg::attach_children");
  DBUG_PRINT("myrg", ("table: '%s'.'%s' %p", table->s->db.str,
                      table->s->table_name.str, table));
  DBUG_PRINT("myrg", ("test_if_locked: %u", this->test_if_locked));

  /* Must call this with open table. */
  DBUG_ASSERT(this->file);

  /*
    A MERGE table with no children (empty union) is always seen as
    attached internally.
  */
  if (!this->file->tables)
  {
    DBUG_PRINT("myrg", ("empty merge table union"));
    goto end;
  }
  DBUG_PRINT("myrg", ("child tables: %u", this->file->tables));

  /* Must not call this with attached children. */
  DBUG_ASSERT(!this->file->children_attached);

  DEBUG_SYNC(current_thd, "before_myisammrg_attach");
  /* Must call this with children list in place. */
  DBUG_ASSERT(this->table->pos_in_table_list->next_global == this->children_l);

  if (myrg_attach_children(this->file, this->test_if_locked |
                           current_thd->open_options,
                           myisammrg_attach_children_callback, &param,
                           (my_bool *) &param.need_compat_check))
  {
    error= my_errno;
    goto err;
  }
  DBUG_PRINT("myrg", ("calling myrg_extrafunc"));
  myrg_extrafunc(file, query_cache_invalidate_by_MyISAM_filename_ref);
  if (!(test_if_locked == HA_OPEN_WAIT_IF_LOCKED ||
	test_if_locked == HA_OPEN_ABORT_IF_LOCKED))
    myrg_extra(file,HA_EXTRA_NO_WAIT_LOCK,0);
  info(HA_STATUS_NO_LOCK | HA_STATUS_VARIABLE | HA_STATUS_CONST);
  if (!(test_if_locked & HA_OPEN_WAIT_IF_LOCKED))
    myrg_extra(file,HA_EXTRA_WAIT_LOCK,0);

  /*
    The compatibility check is required only if one or more children do
    not match their table def version from the last check. This will
    always happen at the first attach because the reference child def
    version is initialized to 'undefined' at open.
  */
  DBUG_PRINT("myrg", ("need_compat_check: %d", param.need_compat_check));
  if (param.need_compat_check)
  {
    TABLE_LIST *child_l;

    if (table->s->reclength != stats.mean_rec_length && stats.mean_rec_length)
    {
      DBUG_PRINT("error",("reclength: %lu  mean_rec_length: %lu",
                          table->s->reclength, stats.mean_rec_length));
      if (test_if_locked & HA_OPEN_FOR_REPAIR)
      {
        /* purecov: begin inspected */
        myrg_print_wrong_table(file->open_tables->table->filename);
        /* purecov: end */
      }
      error= HA_ERR_WRONG_MRG_TABLE_DEF;
      goto err;
    }
    /*
      Both recinfo and keyinfo are allocated by my_multi_malloc(), thus
      only recinfo must be freed.
    */
    if ((error= table2myisam(table, &keyinfo, &recinfo, &recs)))
    {
      /* purecov: begin inspected */
      DBUG_PRINT("error", ("failed to convert TABLE object to MyISAM "
                           "key and column definition"));
      goto err;
      /* purecov: end */
    }
    for (u_table= file->open_tables; u_table < file->end_table; u_table++)
    {
      if (check_definition(keyinfo, recinfo, keys, recs,
                           u_table->table->s->keyinfo, u_table->table->s->rec,
                           u_table->table->s->base.keys,
                           u_table->table->s->base.fields, false, NULL))
      {
        DBUG_PRINT("error", ("table definition mismatch: '%s'",
                             u_table->table->filename));
        error= HA_ERR_WRONG_MRG_TABLE_DEF;
        if (!(this->test_if_locked & HA_OPEN_FOR_REPAIR))
        {
          my_free(recinfo);
          goto err;
        }
        /* purecov: begin inspected */
        myrg_print_wrong_table(u_table->table->filename);
        /* purecov: end */
      }
    }
    my_free(recinfo);
    if (error == HA_ERR_WRONG_MRG_TABLE_DEF)
      goto err; /* purecov: inspected */

    List_iterator_fast<Mrg_child_def> def_it(child_def_list);
    DBUG_ASSERT(this->children_l);
    for (child_l= this->children_l; ; child_l= child_l->next_global)
    {
      Mrg_child_def *mrg_child_def= def_it++;
      mrg_child_def->set_child_def_version(
        child_l->table->s->get_table_ref_type(),
        child_l->table->s->get_table_def_version());

      if (&child_l->next_global == this->children_last_l)
        break;
    }
  }
#if SIZEOF_OFF_T == 4
  /* Merge table has more than 2G rows */
  if (table->s->crashed)
  {
    DBUG_PRINT("error", ("MERGE table marked crashed"));
    error= HA_ERR_WRONG_MRG_TABLE_DEF;
    goto err;
  }
#endif

 end:
  DBUG_RETURN(0);

err:
  DBUG_PRINT("error", ("attaching MERGE children failed: %d", error));
  print_error(error, MYF(0));
  detach_children();
  DBUG_RETURN(my_errno= error);
}


/**
  Detach all children from a MERGE table and from the query list of tables.

  @return status
    @retval     0               OK
    @retval     != 0            Error, my_errno gives reason

  @note
    Detach must not touch the child TABLE objects in any way.
    They may have been closed at ths point already.
    All references to the children should be removed.
*/

int ha_myisammrg::detach_children(void)
{
  TABLE_LIST *child_l;
  DBUG_ENTER("ha_myisammrg::detach_children");

  /* Must call this with open table. */
  DBUG_ASSERT(this->file);

  /* A MERGE table with no children (empty union) cannot be detached. */
  if (!this->file->tables)
  {
    DBUG_PRINT("myrg", ("empty merge table union"));
    goto end;
  }

  if (this->children_l)
  {
    THD *thd= table->in_use;

    /* Clear TABLE references. */
    for (child_l= this->children_l; ; child_l= child_l->next_global)
    {
      /*
        Do not DBUG_ASSERT(child_l->table); open_tables might be
        incomplete.

        Clear the table reference.
      */
      child_l->table= NULL;
      /* Similarly, clear the ticket reference. */
      child_l->mdl_request.ticket= NULL;

      /* Break when this was the last child. */
      if (&child_l->next_global == this->children_last_l)
        break;
    }
    /*
      Remove children from the table list. This won't fail if called
      twice. The list is terminated after removal.

      If the parent is LEX::query_tables_own_last and pre-locked tables
      follow (tables used by stored functions or triggers), the children
      are inserted behind the parent and before the pre-locked tables. But
      we do not adjust LEX::query_tables_own_last. The pre-locked tables
      could have chopped off the list by clearing
      *LEX::query_tables_own_last. This did also chop off the children. If
      we would copy the reference from *this->children_last_l in this
      case, we would put the chopped off pre-locked tables back to the
      list. So we refrain from copying it back, if the destination has
      been set to NULL meanwhile.
    */
    if (this->children_l->prev_global && *this->children_l->prev_global)
      *this->children_l->prev_global= *this->children_last_l;
    if (*this->children_last_l)
      (*this->children_last_l)->prev_global= this->children_l->prev_global;

    /*
      If table elements being removed are at the end of table list we
      need to adjust LEX::query_tables_last member to point to the
      new last element of the list.
    */
    if (thd->lex->query_tables_last == this->children_last_l)
      thd->lex->query_tables_last= this->children_l->prev_global;

    /*
      If the statement requires prelocking, and prelocked
      tables were added right after merge children, modify the
      last own table pointer to point at prev_global of the merge
      parent.
    */
    if (thd->lex->query_tables_own_last == this->children_last_l)
      thd->lex->query_tables_own_last= this->children_l->prev_global;

    /* Terminate child list. So it cannot be tried to remove again. */
    *this->children_last_l= NULL;
    this->children_l->prev_global= NULL;

    /* Forget about the children, we don't own their memory. */
    this->children_l= NULL;
    this->children_last_l= NULL;
  }

  if (!this->file->children_attached)
  {
    DBUG_PRINT("myrg", ("merge children are already detached"));
    goto end;
  }

  if (myrg_detach_children(this->file))
  {
    /* purecov: begin inspected */
    print_error(my_errno, MYF(0));
    DBUG_RETURN(my_errno ? my_errno : -1);
    /* purecov: end */
  }

 end:
  DBUG_RETURN(0);
}


/**
  Close a MERGE parent table, but not its children.

  @return status
    @retval     0               OK
    @retval     != 0            Error, my_errno gives reason

  @note
    The children are expected to be closed separately by the caller.
*/

int ha_myisammrg::close(void)
{
  int rc;
  DBUG_ENTER("ha_myisammrg::close");
  /*
    There are cases where children are not explicitly detached before
    close. detach_children() protects itself against double detach.
  */
  if (!is_cloned)
    detach_children();

  rc= myrg_close(file);
  file= 0;
  DBUG_RETURN(rc);
}

int ha_myisammrg::write_row(const uchar * buf)
{
  DBUG_ENTER("ha_myisammrg::write_row");
  DBUG_ASSERT(this->file->children_attached);

  if (file->merge_insert_method == MERGE_INSERT_DISABLED || !file->tables)
    DBUG_RETURN(HA_ERR_TABLE_READONLY);

  if (table->next_number_field && buf == table->record[0])
  {
    int error;
    if ((error= update_auto_increment()))
      DBUG_RETURN(error); /* purecov: inspected */
  }
  DBUG_RETURN(myrg_write(file,buf));
}

int ha_myisammrg::update_row(const uchar * old_data, const uchar * new_data)
{
  DBUG_ASSERT(this->file->children_attached);
  return myrg_update(file,old_data,new_data);
}

int ha_myisammrg::delete_row(const uchar * buf)
{
  DBUG_ASSERT(this->file->children_attached);
  return myrg_delete(file,buf);
}

int ha_myisammrg::index_read_map(uchar * buf, const uchar * key,
                                 key_part_map keypart_map,
                                 enum ha_rkey_function find_flag)
{
  DBUG_ASSERT(this->file->children_attached);
  int error=myrg_rkey(file,buf,active_index, key, keypart_map, find_flag);
  return error;
}

int ha_myisammrg::index_read_idx_map(uchar * buf, uint index, const uchar * key,
                                     key_part_map keypart_map,
                                     enum ha_rkey_function find_flag)
{
  DBUG_ASSERT(this->file->children_attached);
  int error=myrg_rkey(file,buf,index, key, keypart_map, find_flag);
  return error;
}

int ha_myisammrg::index_read_last_map(uchar *buf, const uchar *key,
                                      key_part_map keypart_map)
{
  DBUG_ASSERT(this->file->children_attached);
  int error=myrg_rkey(file,buf,active_index, key, keypart_map,
		      HA_READ_PREFIX_LAST);
  return error;
}

int ha_myisammrg::index_next(uchar * buf)
{
  DBUG_ASSERT(this->file->children_attached);
  int error=myrg_rnext(file,buf,active_index);
  return error;
}

int ha_myisammrg::index_prev(uchar * buf)
{
  DBUG_ASSERT(this->file->children_attached);
  int error=myrg_rprev(file,buf, active_index);
  return error;
}

int ha_myisammrg::index_first(uchar * buf)
{
  DBUG_ASSERT(this->file->children_attached);
  int error=myrg_rfirst(file, buf, active_index);
  return error;
}

int ha_myisammrg::index_last(uchar * buf)
{
  DBUG_ASSERT(this->file->children_attached);
  int error=myrg_rlast(file, buf, active_index);
  return error;
}

int ha_myisammrg::index_next_same(uchar * buf,
                                  const uchar *key __attribute__((unused)),
                                  uint length __attribute__((unused)))
{
  int error;
  DBUG_ASSERT(this->file->children_attached);
  do
  {
    error= myrg_rnext_same(file,buf);
  } while (error == HA_ERR_RECORD_DELETED);
  return error;
}


int ha_myisammrg::rnd_init(bool scan)
{
  DBUG_ASSERT(this->file->children_attached);
  return myrg_reset(file);
}


int ha_myisammrg::rnd_next(uchar *buf)
{
  DBUG_ASSERT(this->file->children_attached);
  int error=myrg_rrnd(file, buf, HA_OFFSET_ERROR);
  return error;
}


int ha_myisammrg::rnd_pos(uchar * buf, uchar *pos)
{
  DBUG_ASSERT(this->file->children_attached);
  int error=myrg_rrnd(file, buf, my_get_ptr(pos,ref_length));
  return error;
}

void ha_myisammrg::position(const uchar *record)
{
  DBUG_ASSERT(this->file->children_attached);
  ulonglong row_position= myrg_position(file);
  my_store_ptr(ref, ref_length, (my_off_t) row_position);
}


ha_rows ha_myisammrg::records_in_range(uint inx,
                                       const key_range *min_key,
                                       const key_range *max_key,
                                       page_range *pages)
{
  DBUG_ASSERT(this->file->children_attached);
  return (ha_rows) myrg_records_in_range(file, (int) inx, min_key, max_key,
                                         pages);
}


int ha_myisammrg::delete_all_rows()
{
  int err= 0;
  MYRG_TABLE *table;
  DBUG_ENTER("ha_myisammrg::delete_all_rows");

  for (table= file->open_tables; table != file->end_table; table++)
  {
    if ((err= mi_delete_all_rows(table->table)))
      break;
  }

  DBUG_RETURN(err);
}


int ha_myisammrg::info(uint flag)
{
  MYMERGE_INFO mrg_info;
  (void) myrg_status(file,&mrg_info,flag);
  /*
    The following fails if one has not compiled MySQL with -DBIG_TABLES
    and one has more than 2^32 rows in the merge tables.
  */
  stats.records = (ha_rows) mrg_info.records;
  stats.deleted = (ha_rows) mrg_info.deleted;
#if SIZEOF_OFF_T == 4
  if ((mrg_info.records >= (ulonglong) 1 << 32) ||
      (mrg_info.deleted >= (ulonglong) 1 << 32))
    table->s->crashed= 1;
#endif
  stats.data_file_length= mrg_info.data_file_length;
  if (mrg_info.errkey >= (int) table_share->keys)
  {
    /*
     If value of errkey is higher than the number of keys
     on the table set errkey to MAX_KEY. This will be
     treated as unknown key case and error message generator
     won't try to locate key causing segmentation fault.
    */
    mrg_info.errkey= MAX_KEY;
  }
  table->s->keys_in_use.set_prefix(table->s->keys);
  stats.mean_rec_length= mrg_info.reclength;
  
  /*
    The handler::block_size is used all over the code in index scan cost
    calculations. It is used to get number of disk seeks required to
    retrieve a number of index tuples.
    If the merge table has N underlying tables, there will be
    N more disk seeks compared to a scanning a normal MyISAM table.
    The number of bytes read is the rougly the same for a normal MyISAM
    and a MyISAM merge tables.
  */
  stats.block_size= myisam_block_size;

  stats.update_time= 0;
#if SIZEOF_OFF_T > 4
  ref_length=6;					// Should be big enough
#else
  ref_length=4;					// Can't be > than my_off_t
#endif
  if (flag & HA_STATUS_CONST)
  {
    if (table->s->key_parts && mrg_info.rec_per_key)
    {
#ifdef HAVE_valgrind
      /*
        valgrind may be unhappy about it, because optimizer may access values
        between file->keys and table->key_parts, that will be uninitialized.
        It's safe though, because even if optimizer will decide to use a key
        with such a number, it'll be an error later anyway.
      */
      bzero((char*) table->key_info[0].rec_per_key,
            sizeof(table->key_info[0].rec_per_key[0]) * table->s->key_parts);
#endif
      memcpy((char*) table->key_info[0].rec_per_key,
	     (char*) mrg_info.rec_per_key,
             sizeof(table->key_info[0].rec_per_key[0]) *
             MY_MIN(file->keys, table->s->key_parts));
    }
  }
  if (flag & HA_STATUS_ERRKEY)
  {
    errkey= mrg_info.errkey;
    my_store_ptr(dup_ref, ref_length, mrg_info.dupp_key_pos);
  }
  return 0;
}


int ha_myisammrg::extra(enum ha_extra_function operation)
{
  if (operation == HA_EXTRA_ADD_CHILDREN_LIST)
  {
    int rc= add_children_list();
    return(rc);
  }
  else if (operation == HA_EXTRA_ATTACH_CHILDREN)
  {
    int rc= attach_children();
    if (!rc)
      (void) extra(HA_EXTRA_NO_READCHECK); // Not needed in SQL
    return(rc);
  }
  else if (operation == HA_EXTRA_IS_ATTACHED_CHILDREN)
  {
    /* For the upper layer pretend empty MERGE union is never attached. */
    return(file && file->tables && file->children_attached);
  }
  else if (operation == HA_EXTRA_DETACH_CHILDREN)
  {
    /*
      Note that detach must not touch the children in any way.
      They may have been closed at ths point already.
    */
    int rc= detach_children();
    return(rc);
  }

  /* As this is just a mapping, we don't have to force the underlying
     tables to be closed */
  if (operation == HA_EXTRA_FORCE_REOPEN ||
      operation == HA_EXTRA_PREPARE_FOR_DROP ||
      operation == HA_EXTRA_PREPARE_FOR_RENAME)
    return 0;
  if (operation == HA_EXTRA_MMAP && !opt_myisam_use_mmap)
    return 0;
  return myrg_extra(file,operation,0);
}

int ha_myisammrg::reset(void)
{
  /* This is normally called with detached children. */
  return myrg_reset(file);
}

/* To be used with WRITE_CACHE, EXTRA_CACHE and BULK_INSERT_BEGIN */

int ha_myisammrg::extra_opt(enum ha_extra_function operation, ulong cache_size)
{
  DBUG_ASSERT(this->file->children_attached);
  return myrg_extra(file, operation, (void*) &cache_size);
}

int ha_myisammrg::external_lock(THD *thd, int lock_type)
{
  /*
    This can be called with no children attached. E.g. FLUSH TABLES
    unlocks and re-locks tables under LOCK TABLES, but it does not open
    them first. So they are detached all the time. But locking of the
    children should work anyway because thd->open_tables is not changed
    during FLUSH TABLES.

    If this handler instance has been cloned, we still must call
    myrg_lock_database().
  */
  if (is_cloned)
    return myrg_lock_database(file, lock_type);
  return 0;
}

uint ha_myisammrg::lock_count(void) const
{
  return 0;
}


THR_LOCK_DATA **ha_myisammrg::store_lock(THD *thd,
					 THR_LOCK_DATA **to,
					 enum thr_lock_type lock_type)
{
  MYRG_TABLE *open_table;

  /*
    This method can be called while another thread is attaching the
    children. If the processor reorders instructions or write to memory,
    'children_attached' could be set before 'open_tables' has all the
    pointers to the children. Use of a mutex here and in
    myrg_attach_children() forces consistent data.
  */
  mysql_mutex_lock(&this->file->mutex);

  /*
    When MERGE table is open, but not yet attached, other threads
    could flush it, which means calling mysql_lock_abort_for_thread()
    on this threads TABLE. 'children_attached' is FALSE in this
    situation. Since the table is not locked, return no lock data.
  */
  if (!this->file->children_attached)
    goto end; /* purecov: tested */

  for (open_table=file->open_tables ;
       open_table != file->end_table ;
       open_table++)
    open_table->table->lock.priority|= THR_LOCK_MERGE_PRIV;

 end:
  mysql_mutex_unlock(&this->file->mutex);
  return to;
}


/* Find out database name and table name from a filename */

static void split_file_name(const char *file_name,
			    LEX_STRING *db, LEX_STRING *name)
{
  size_t dir_length, prefix_length;
  char buff[FN_REFLEN];

  db->length= 0;
  strmake_buf(buff, file_name);
  dir_length= dirname_length(buff);
  if (dir_length > 1)
  {
    /* Get database */
    buff[dir_length-1]= 0;			// Remove end '/'
    prefix_length= dirname_length(buff);
    db->str= (char*) file_name+ prefix_length;
    db->length= dir_length - prefix_length -1;
  }
  name->str= (char*) file_name+ dir_length;
  name->length= (uint) (fn_ext(name->str) - name->str);
}


void ha_myisammrg::update_create_info(HA_CREATE_INFO *create_info)
{
  DBUG_ENTER("ha_myisammrg::update_create_info");

  if (!(create_info->used_fields & HA_CREATE_USED_UNION))
  {
    TABLE_LIST *child_table, *end= NULL;
    THD *thd=ha_thd();

    if (children_l != NULL)
    {
      for (child_table= children_l;; child_table= child_table->next_global)
      {
        TABLE_LIST *ptr;

        if (!(ptr= thd->calloc<TABLE_LIST>(1)))
          DBUG_VOID_RETURN;

        if (!(ptr->table_name.str= thd->strmake(child_table->table_name.str,
                                                child_table->table_name.length)))
          DBUG_VOID_RETURN;
        ptr->table_name.length= child_table->table_name.length;
        if (child_table->db.str && !(ptr->db.str= thd->strmake(child_table->db.str,
                                                               child_table->db.length)))
          DBUG_VOID_RETURN;
        ptr->db.length= child_table->db.length;

        if (create_info->merge_list)
          end->next_local= ptr;
        else
          create_info->merge_list= ptr;
        end= ptr;

        if (&child_table->next_global == children_last_l)
          break;
      }
    }
  }
  if (!(create_info->used_fields & HA_CREATE_USED_INSERT_METHOD))
  {
    create_info->merge_insert_method = file->merge_insert_method;
  }
  DBUG_VOID_RETURN;
}


int ha_myisammrg::create_mrg(const char *name, HA_CREATE_INFO *create_info)
{
  char buff[FN_REFLEN];
  const char **table_names, **pos;
  TABLE_LIST *tables= create_info->merge_list;
  THD *thd= ha_thd();
  size_t dirlgt= dirname_length(name);
  uint ntables= 0;
  DBUG_ENTER("ha_myisammrg::create_mrg");

  for (tables= create_info->merge_list; tables; tables= tables->next_local)
    ntables++;

  /* Allocate a table_names array in thread mem_root. */
  if (!(pos= table_names= thd->alloc<const char*>(ntables + 1)))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM); /* purecov: inspected */

  /* Create child path names. */
  for (tables= create_info->merge_list; tables; tables= tables->next_local)
  {
    const char *table_name= buff;

    /*
      Construct the path to the MyISAM table. Try to meet two conditions:
      1.) Allow to include MyISAM tables from different databases, and
      2.) allow for moving DATADIR around in the file system.
      The first means that we need paths in the .MRG file. The second
      means that we should not have absolute paths in the .MRG file.
      The best, we can do, is to use 'mysql_data_home', which is '.'
      in mysqld and may be an absolute path in an embedded server.
      This means that it might not be possible to move the DATADIR of
      an embedded server without changing the paths in the .MRG file.

      Do the same even for temporary tables. MERGE children are now
      opened through the table cache. They are opened by db.table_name,
      not by their path name.
    */
    size_t length= build_table_filename(buff, sizeof(buff),
                                      tables->db.str, tables->table_name.str, "", 0);
    /*
      If a MyISAM table is in the same directory as the MERGE table,
      we use the table name without a path. This means that the
      DATADIR can easily be moved even for an embedded server as long
      as the MyISAM tables are from the same database as the MERGE table.
    */
    if ((dirname_length(buff) == dirlgt) && ! memcmp(buff, name, dirlgt))
    {
      table_name+= dirlgt;
      length-= dirlgt;
    }
    if (!(table_name= thd->strmake(table_name, length)))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM); /* purecov: inspected */

    *pos++= table_name;
  }
  *pos=0;

  /* Create a MERGE meta file from the table_names array. */
  int res= myrg_create(name, table_names, create_info->merge_insert_method, 0);
  DBUG_RETURN(res);
}


int ha_myisammrg::create(const char *name, TABLE *form,
			 HA_CREATE_INFO *create_info)
{
  char buff[FN_REFLEN];
  DBUG_ENTER("ha_myisammrg::create");
  if (form->s->total_keys > form->s->keys)
  {
    my_error(ER_ILLEGAL_HA_CREATE_OPTION, MYF(0), "MERGE", "VECTOR");
    DBUG_RETURN(HA_ERR_UNSUPPORTED);
  }
  fn_format(buff, name, "", MYRG_NAME_EXT, MY_UNPACK_FILENAME | MY_APPEND_EXT);
  int res= create_mrg(buff, create_info);
  DBUG_RETURN(res);
}


void ha_myisammrg::append_create_info(String *packet)
{
  const char *current_db;
  size_t db_length;
  THD *thd= current_thd;
  TABLE_LIST *open_table, *first;

  if (file->merge_insert_method != MERGE_INSERT_DISABLED)
  {
    const char *type;
    packet->append(STRING_WITH_LEN(" INSERT_METHOD="));
    type= get_type(&merge_insert_method,file->merge_insert_method-1);
    packet->append(type, strlen(type));
  }
  /*
    There is no sence adding UNION clause in case there is no underlying
    tables specified.
  */
  if (file->open_tables == file->end_table)
    return;
  packet->append(STRING_WITH_LEN(" UNION=("));

  current_db= table->s->db.str;
  db_length=  table->s->db.length;

  for (first= open_table= children_l;;
       open_table= open_table->next_global)
  {
    LEX_CSTRING db= open_table->db;

    if (open_table != first)
      packet->append(',');
    /* Report database for mapped table if it isn't in current database */
    if (db.length &&
	(db_length != db.length ||
	 strncmp(current_db, db.str, db.length)))
    {
      append_identifier(thd, packet, db.str, db.length);
      packet->append('.');
    }
    append_identifier(thd, packet, &open_table->table_name);
    if (&open_table->next_global == children_last_l)
      break;
  }
  packet->append(')');
}


enum_alter_inplace_result
ha_myisammrg::check_if_supported_inplace_alter(TABLE *altered_table,
                                               Alter_inplace_info *ha_alter_info)
{
  /*
    We always support inplace ALTER in the new API, because old
    HA_NO_COPY_ON_ALTER table_flags() hack prevents non-inplace ALTER anyway.
  */
  return HA_ALTER_INPLACE_EXCLUSIVE_LOCK;
}


bool ha_myisammrg::inplace_alter_table(TABLE *altered_table,
                                       Alter_inplace_info *ha_alter_info)
{
  char tmp_path[FN_REFLEN];
  const char *name= table->s->normalized_path.str;
  DBUG_ENTER("ha_myisammrg::inplace_alter_table");
  fn_format(tmp_path, name, "", MYRG_NAME_TMPEXT, MY_UNPACK_FILENAME | MY_APPEND_EXT);
  int res= create_mrg(tmp_path, ha_alter_info->create_info);
  if (res)
    mysql_file_delete(rg_key_file_MRG, tmp_path, MYF(0));
  else
  {
    char path[FN_REFLEN];
    fn_format(path, name, "", MYRG_NAME_EXT, MY_UNPACK_FILENAME | MY_APPEND_EXT);
    if (mysql_file_rename(rg_key_file_MRG, tmp_path, path, MYF(0)))
    {
      res= my_errno;
      mysql_file_delete(rg_key_file_MRG, tmp_path, MYF(0));
    }
  }
  DBUG_RETURN(res);
}

int ha_myisammrg::check(THD* thd, HA_CHECK_OPT* check_opt)
{
  return this->file->children_attached ? HA_ADMIN_OK : HA_ADMIN_CORRUPT;
}


ha_rows ha_myisammrg::records()
{
  return myrg_records(file);
}

uint ha_myisammrg::count_query_cache_dependant_tables(uint8 *tables_type)
{
  MYRG_INFO *file = myrg_info();
  /*
    Here should be following statement
  (*tables_type)|= HA_CACHE_TBL_NONTRANSACT;
    but it has no effect because HA_CACHE_TBL_NONTRANSACT is 0
  */
  return (uint)(file->end_table - file->open_tables);
}


my_bool ha_myisammrg::register_query_cache_dependant_tables(THD *thd
                                          __attribute__((unused)),
                                          Query_cache *cache,
                                          Query_cache_block_table **block_table,
                                          uint *n)
{
  MYRG_INFO *file =myrg_info();
  DBUG_ENTER("ha_myisammrg::register_query_cache_dependant_tables");

  for (MYRG_TABLE *table =file->open_tables;
       table != file->end_table ;
       table++)
  {
    char key[MAX_DBKEY_LENGTH];
    uint32 db_length;
    uint key_length= cache->filename_2_table_key(key, table->table->filename,
                                                 &db_length);
    (++(*block_table))->n= ++(*n);
    /*
      There are not callback function for for MyISAM, and engine data
    */
    if (!cache->insert_table(thd, key_length, key, (*block_table),
                             db_length, 0,
                             table_cache_type(),
                             0, 0, TRUE))
      DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}


void ha_myisammrg::set_lock_type(enum thr_lock_type lock)
{
  handler::set_lock_type(lock);
  if (children_l != NULL)
  {
    for (TABLE_LIST *child_table= children_l;;
         child_table= child_table->next_global)
    {
      child_table->lock_type=
        child_table->table->reginfo.lock_type= lock;

      if (&child_table->next_global == children_last_l)
        break;
    }
  }
}

extern int myrg_panic(enum ha_panic_function flag);
int myisammrg_panic(handlerton *hton, ha_panic_function flag)
{
  return myrg_panic(flag);
}

static void myisammrg_update_optimizer_costs(OPTIMIZER_COSTS *costs)
{
  myisam_update_optimizer_costs(costs);
}


static int myisammrg_init(void *p)
{
  handlerton *myisammrg_hton;

  myisammrg_hton= (handlerton *)p;

#ifdef HAVE_PSI_INTERFACE
  init_myisammrg_psi_keys();
#endif

  myisammrg_hton->db_type= DB_TYPE_MRG_MYISAM;
  myisammrg_hton->create= myisammrg_create_handler;
  myisammrg_hton->panic= myisammrg_panic;
  myisammrg_hton->flags= HTON_NO_PARTITION;
  myisammrg_hton->tablefile_extensions= ha_myisammrg_exts;
  myisammrg_hton->update_optimizer_costs= myisammrg_update_optimizer_costs;
  return 0;
}

struct st_mysql_storage_engine myisammrg_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(myisammrg)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &myisammrg_storage_engine,
  "MRG_MyISAM",
  "MySQL AB",
  "Collection of identical MyISAM tables",
  PLUGIN_LICENSE_GPL,
  myisammrg_init, /* Plugin Init */
  NULL, /* Plugin Deinit */
  0x0100, /* 1.0 */
  NULL,                       /* status variables                */
  NULL,                       /* system variables                */
  "1.0",                      /* string version */
  MariaDB_PLUGIN_MATURITY_STABLE /* maturity */
}
maria_declare_plugin_end;
