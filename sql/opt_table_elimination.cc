/*
   Copyright (c) 2009, 2011, Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/**
  @file

  @brief
    Table Elimination Module

  @defgroup Table_Elimination Table Elimination Module
  @{
*/

#include "mariadb.h"
#include "my_bit.h"
#include "sql_select.h"
#include "opt_trace.h"
#include "my_json_writer.h"

/*
  OVERVIEW
  ========

  This file contains table elimination module. The idea behind table
  elimination is as follows: suppose we have a left join
 
    SELECT * FROM t1 LEFT JOIN 
      (t2 JOIN t3) ON t2.primary_key=t1.col AND 
                      t2.primary_key=t2.col
    WHERE ...

  such that
  * columns of the inner tables are not used anywhere ouside the outer join
    (not in WHERE, not in GROUP/ORDER BY clause, not in select list etc etc),
  * inner side of the outer join is guaranteed to produce at most one matching
    record combination for each record combination of outer tables.
  
  then the inner side of the outer join can be removed from the query, as it 
  will always produce only one record combination (either real or 
  null-complemented one) and we don't care about what that record combination 
  is.


  MODULE INTERFACE
  ================

  The module has one entry point - the eliminate_tables() function, which one
  needs to call (once) at some point before join optimization.
  eliminate_tables() operates over the JOIN structures. Logically, it
  removes the inner tables of an outer join operation together with the
  operation itself. Physically, it changes the following members:

  * Eliminated tables are marked as constant and moved to the front of the
    join order.

  * In addition to this, they are recorded in JOIN::eliminated_tables bitmap.

  * Items that became disused because they were in the ON expression of an 
    eliminated outer join are notified by means of the Item tree walk which 
    calls Item::mark_as_eliminated_processor for every item
    - At the moment the only Item that cares whether it was eliminated is 
      Item_subselect with its Item_subselect::eliminated flag which is used
      by EXPLAIN code to check if the subquery should be shown in EXPLAIN.

  Table elimination is redone on every PS re-execution.


  TABLE ELIMINATION ALGORITHM FOR ONE OUTER JOIN
  ==============================================

  As described above, we can remove inner side of an outer join if it is 

    1. not referred to from any other parts of the query
    2. always produces one matching record combination.

  We check #1 by doing a recursive descent down the join->join_list while
  maintaining a union of used_tables() attribute of all Item expressions in
  other parts of the query. When we encounter an outer join, we check if the
  bitmap of tables on its inner side has intersection with tables that are used
  elsewhere. No intersection means that inner side of the outer join could 
  potentially be eliminated.

  In order to check #2, one needs to prove that inner side of an outer join 
  is functionally dependent on the outside. The proof is constructed from
  functional dependencies of intermediate objects:

  - Inner side of outer join is functionally dependent when each of its tables
    are functionally dependent. (We assume a table is functionally dependent 
    when its dependencies allow to uniquely identify one table record, or no
    records).

  - Table is functionally dependent when it has got a unique key whose columns
    are functionally dependent.

  - A column is functionally dependent when we could locate an AND-part of a
    certain ON clause in form 
      
      tblX.columnY= expr 
    
    where expr is functionally depdendent. expr is functionally dependent when 
    all columns that it refers to are functionally dependent.

  These relationships are modeled as a bipartite directed graph that has
  dependencies as edges and two kinds of nodes:

  Value nodes:
   - Table column values (each is a value of tblX.columnY)
   - Table values (each node represents a table inside the join nest we're
     trying to eliminate).
  A value has one attribute, it is either bound (i.e. functionally dependent) 
  or not.

  Module nodes:
   - Modules representing tblX.colY=expr equalities. Equality module has 
      = incoming edges from columns used in expr 
      = outgoing edge to tblX.colY column.
   - Nodes representing unique keys. Unique key has
      = incoming edges from key component value modules
      = outgoing edge to key's table module
   - Nodes representing unique pseudo-keys for derived tables.
     Unique pseudo-keys are composed as a result of GROUP BY expressions.
     Like normal unique keys, they have:
      = incoming edges from key component value modules
      = outgoing edge to key's table module
   - Inner side of outer join module. Outer join module has
      = incoming edges from table value modules
      = No outgoing edges. Once we reach it, we know we can eliminate the 
        outer join.
  A module may depend on multiple values, and hence its primary attribute is
  the number of its arguments that are not bound. 

  The algorithm starts with equality nodes that don't have any incoming edges
  (their expressions are either constant or depend only on tables that are
  outside of the outer join in question) and performs a breadth-first
  traversal. If we reach the outer join nest node, it means outer join is
  functionally dependent and can be eliminated. Otherwise it cannot be
  eliminated.
 
  HANDLING MULTIPLE NESTED OUTER JOINS
  ====================================

  Outer joins that are not nested one within another are eliminated
  independently. For nested outer joins we have the following considerations:
  
  1. ON expressions from children outer joins must be taken into account 
   
  Consider this example:

    SELECT t0.* 
    FROM 
      t0  
    LEFT JOIN 
      (t1 LEFT JOIN t2 ON t2.primary_key=t1.col1)
    ON 
      t1.primary_key=t0.col AND t2.col1=t1.col2

  Here we cannot eliminate the "... LEFT JOIN t2 ON ..." part alone because the
  ON clause of top level outer join has references to table t2. 
  We can eliminate the entire  "... LEFT JOIN (t1 LEFT JOIN t2) ON .." part,
  but in order to do that, we must look at both ON expressions.
  
  2. ON expressions of parent outer joins are useless.
  Consider an example:

    SELECT t0.* 
    FROM
      t0 
    LEFT JOIN 
      (t1 LEFT JOIN t2 ON some_expr)
    ON
      t2.primary_key=t1.col  -- (*)
  
  Here the uppermost ON expression has a clause that gives us functional
  dependency of table t2 on t1 and hence could be used to eliminate the
  "... LEFT JOIN t2 ON..." part.
  However, we would not actually encounter this situation, because before the
  table elimination we run simplify_joins(), which, among other things, upon
  seeing a functional dependency condition like (*) will convert the outer join
  of
    
    "... LEFT JOIN t2 ON ..."
  
  into inner join and thus make table elimination not to consider eliminating
  table t2.
*/

class Dep_value;
  class Dep_value_field;
  class Dep_value_table;
 

class Dep_module;
  class Dep_module_expr;
  class Dep_module_goal;
  class Dep_module_key;
  class Dep_module_pseudo_key;

class Dep_analysis_context;


/*
  A value, something that can be bound or not bound. One can also iterate over 
  unbound modules that depend on this value
*/

class Dep_value : public Sql_alloc
{
public:
  Dep_value(): bound(FALSE) {}
  virtual ~Dep_value() = default; /* purecov: inspected */
  
  bool is_bound() { return bound; }
  void make_bound() { bound= TRUE; }

  /* Iteration over unbound modules that depend on this value */
  typedef char *Iterator;
  virtual Iterator init_unbound_modules_iter(char *buf)=0;
  virtual Dep_module* get_next_unbound_module(Dep_analysis_context *dac,
                                              Iterator iter) = 0;
  static const size_t iterator_size;
protected:
  bool bound;
};


/*
  A table field value. There is exactly only one such object for any tblX.fieldY
  - the field depends on its table and equalities
  - expressions that use the field are its dependencies
*/

class Dep_value_field : public Dep_value
{
public:
  Dep_value_field(Dep_value_table *table_arg, Field *field_arg) :
    table(table_arg), field(field_arg)
  {}

  Dep_value_table *table; /* Table this field is from */
  Field *field; /* Field this object is representing */
  
  /* Iteration over unbound modules that are our dependencies */
  Iterator init_unbound_modules_iter(char *buf) override;
  Dep_module* get_next_unbound_module(Dep_analysis_context *dac, 
                                      Iterator iter) override;
  
  void make_unbound_modules_iter_skip_keys(Iterator iter);
  
  static const size_t iterator_size;
private:
  /* 
    Field_deps that belong to one table form a linked list, ordered by
    field_index 
  */
  Dep_value_field *next_table_field;

  /*
    Offset to bits in Dep_analysis_context::expr_deps (see comment to that 
    member for semantics of the bits).
  */
  uint bitmap_offset;

  class Module_iter
  {
  public:
    /* if not null, return this and advance */
    Dep_module_key *key_dep;
    /* Otherwise, this and advance */
    uint equality_no;
    /* Or this one and advance */
    Dep_module_pseudo_key *pseudo_key_dep;
  };
  friend class Dep_analysis_context;
  friend class Field_dependency_recorder; 
  friend class Dep_value_table;
};

const size_t Dep_value_field::iterator_size=
  ALIGN_SIZE(sizeof(Dep_value_field::Module_iter));


/*
  A table value. There is one Dep_value_table object for every table that can
  potentially be eliminated.

  Table becomes bound as soon as some of its unique keys becomes bound
  Once the table is bound:
   - all of its fields are bound
   - its embedding outer join has one less unknown argument
*/

class Dep_value_table : public Dep_value
{
public:
  Dep_value_table(TABLE *table_arg) : 
    table(table_arg), fields(NULL), keys(NULL), pseudo_key(NULL)
  {}
  TABLE *table;  /* Table this object is representing */
  /* Ordered list of fields that belong to this table */
  Dep_value_field *fields;

  /* Ordered list of Unique keys in this table */
  Dep_module_key *keys;

  /*
    Possible unique pseudo-key applicable for this table
    (only none or a single one is possible)
  */
  Dep_module_pseudo_key *pseudo_key;

  /* Iteration over unbound modules that are our dependencies */
  Iterator init_unbound_modules_iter(char *buf) override;
  Dep_module* get_next_unbound_module(Dep_analysis_context *dac, 
                                      Iterator iter) override;
  static const size_t iterator_size;
private:
  class Module_iter
  {
  public:
    /* Space for field iterator */
    char buf[Dep_value_field::iterator_size];
    /* !NULL <=> iterating over dependent modules of this field */
    Dep_value_field *field_dep; 
    bool returned_goal;
  };
};


const size_t Dep_value_table::iterator_size=
  ALIGN_SIZE(sizeof(Dep_value_table::Module_iter));

const size_t Dep_value::iterator_size=
  MY_MAX(Dep_value_table::iterator_size, Dep_value_field::iterator_size);


/*
  A 'module'. Module has unsatisfied dependencies, number of whose is stored in
  unbound_args. Modules also can be linked together in a list.
*/

class Dep_module : public Sql_alloc
{
public:
  virtual ~Dep_module() = default;  /* purecov: inspected */
  
  /* Mark as bound. Currently is non-virtual and does nothing */
  void make_bound() {};

  /* 
    The final module will return TRUE here. When we see that TRUE was returned,
    that will mean that functional dependency check succeeded.
  */
  virtual bool is_final () { return FALSE; }

  /* 
    Increment number of bound arguments. this is expected to change
    is_applicable() from false to true after sufficient set of arguments is
    bound.
  */
  void touch() { unbound_args--; }
  bool is_applicable() { return !MY_TEST(unbound_args); }
  
  /* Iteration over values that */
  typedef char *Iterator;
  virtual Iterator init_unbound_values_iter(char *buf)=0;
  virtual Dep_value* get_next_unbound_value(Dep_analysis_context *dac,
                                            Iterator iter)=0;
  static const size_t iterator_size;
protected:
  uint unbound_args;
  
  Dep_module() : unbound_args(0) {}
  /* to bump unbound_args when constructing dependencies */
  friend class Field_dependency_recorder; 
  friend class Dep_analysis_context;
};


/*
  This represents either
   - "tbl.column= expr" equality dependency, i.e. tbl.column depends on fields
     used in the expression, or
   - tbl1.col1=tbl2.col2=... multi-equality.
*/

class Dep_module_expr : public Dep_module
{
public:
  Dep_value_field *field;
  Item  *expr;
  
  List<Dep_value_field> *mult_equal_fields;
  /* Used during condition analysis only, similar to KEYUSE::level */
  uint level;

  Iterator init_unbound_values_iter(char *buf) override;
  Dep_value* get_next_unbound_value(Dep_analysis_context *dac, Iterator iter) override;
  static const size_t iterator_size;
private:
  class Value_iter
  {
  public:
    Dep_value_field *field;
    List_iterator<Dep_value_field> it;
  };
};

const size_t Dep_module_expr::iterator_size=
  ALIGN_SIZE(sizeof(Dep_module_expr::Value_iter));


/*
  A Unique key module
   - Unique key has all of its components as arguments
   - Once unique key is bound, its table value is known
*/

class Dep_module_key: public Dep_module
{
public:
  Dep_module_key(Dep_value_table *table_arg, uint keyno_arg, uint n_parts_arg) :
    table(table_arg), keyno(keyno_arg), next_table_key(NULL)
  {
    unbound_args= n_parts_arg;
  }
  Dep_value_table *table; /* Table this key is from */
  uint keyno;  /* The index we're representing */
  /* Unique keys form a linked list, ordered by keyno */
  Dep_module_key *next_table_key;
  
  Iterator init_unbound_values_iter(char *buf) override;
  Dep_value* get_next_unbound_value(Dep_analysis_context *dac, Iterator iter) override;
  static const size_t iterator_size;
private:
  class Value_iter
  {
  public:
    Dep_value_table *table;
  };
};

const size_t Dep_module_key::iterator_size= 
  ALIGN_SIZE(sizeof(Dep_module_key::Value_iter));


/*
  A unique pseudo-key module for a derived table.
  For example, a derived table
  "SELECT a, count(*) from t1 GROUP BY a"
  has unique values in its first field "a" due to GROUP BY expression
  so this can be considered as a unique key for this derived table
*/

class Dep_module_pseudo_key : public Dep_module
{
public:
  Dep_module_pseudo_key(Dep_value_table *table_arg,
                        MY_BITMAP *exposed_fields,
                        uint exposed_fields_num)
      : table(table_arg), exposed_fields_map(exposed_fields)
  {
    unbound_args= exposed_fields_num;
  }

  Dep_value_table *table;

  Iterator init_unbound_values_iter(char *buf) override;

  Dep_value *get_next_unbound_value(Dep_analysis_context *dac,
                                    Iterator iter) override;

  bool covers_field(int field_index);

  static const size_t iterator_size;

private:
  /*
    Bitmap of field numbers in the derived table's SELECT list
    which are included in the GROUP BY expression.
    For example, unique pseudo-key for SQL
    "SELECT count(*), b, a FROM t1 GROUP BY a, b"
    will include two elements: {2} and {1}, since "a" and "b" are on the
    GROUP BY list and also are present on the SELECT list with numbers 2 and 1
    (numeration starts from 0).
  */
  MY_BITMAP *exposed_fields_map;

  class Value_iter
  {
  public:
    Dep_value_table *table;
  };
};

const size_t Dep_module_pseudo_key::iterator_size=
  ALIGN_SIZE(sizeof(Dep_module_pseudo_key::Value_iter));

const size_t Dep_module::iterator_size=
  MY_MAX(Dep_module_expr::iterator_size,
         MY_MAX(Dep_module_key::iterator_size,
                Dep_module_pseudo_key::iterator_size));

/*
  A module that represents outer join that we're trying to eliminate. If we 
  manage to declare this module to be bound, then outer join can be eliminated.
*/

class Dep_module_goal: public Dep_module
{
public:
  Dep_module_goal(uint n_children)  
  {
    unbound_args= n_children;
  }
  bool is_final() override { return TRUE; }
  /* 
    This is the goal module, so the running wave algorithm should terminate
    once it sees that this module is applicable and should never try to apply
    it, hence no use for unbound value iterator implementation.
  */
  Iterator init_unbound_values_iter(char *buf) override
  { 
    DBUG_ASSERT(0); 
    return NULL;
  }
  Dep_value* get_next_unbound_value(Dep_analysis_context *dac, Iterator iter) override
  {
    DBUG_ASSERT(0); 
    return NULL;
  }
};


/*
  Functional dependency analyzer context
*/
class Dep_analysis_context
{
public:
  bool setup_equality_modules_deps(List<Dep_module> *bound_modules);
  bool run_wave(List<Dep_module> *new_bound_modules);

  /* Tables that we're looking at eliminating */
  table_map usable_tables;
  
  /* Array of equality dependencies */
  Dep_module_expr *equality_mods;
  uint n_equality_mods; /* Number of elements in the array */
  uint n_equality_mods_alloced;

  /* tablenr -> Dep_value_table* mapping. */
  Dep_value_table *table_deps[MAX_KEY];
  
  /* Element for the outer join we're attempting to eliminate */
  Dep_module_goal *outer_join_dep;

  /* 
    Bitmap of how expressions depend on bits. Given a Dep_value_field object,
    one can check bitmap_is_set(expr_deps, field_val->bitmap_offset + expr_no)
    to see if expression equality_mods[expr_no] depends on the given field.
  */
  MY_BITMAP expr_deps;

  Dep_value_table *create_table_value(TABLE_LIST *table_list);
  Dep_value_field *get_field_value(Field *field);

#ifndef DBUG_OFF
  void dbug_print_deps();
#endif 

private:
  void create_unique_pseudo_key_if_needed(TABLE_LIST *table_list,
                                          Dep_value_table *tbl_dep);
  int find_field_in_list(List<Item> &fields_list, Item *field);
};


void eliminate_tables(JOIN *join);

static bool
eliminate_tables_for_list(JOIN *join, 
                          List<TABLE_LIST> *join_list,
                          table_map tables_in_list,
                          Item *on_expr,
                          table_map tables_used_elsewhere,
                          Json_writer_array* trace_eliminate_tables);
static
bool check_func_dependency(JOIN *join, 
                           table_map dep_tables,
                           List_iterator<TABLE_LIST> *it, 
                           TABLE_LIST *oj_tbl,
                           Item* cond);
static 
void build_eq_mods_for_cond(THD *thd, Dep_analysis_context *dac,
                            Dep_module_expr **eq_mod, uint *and_level, 
                            Item *cond);
static 
void check_equality(Dep_analysis_context *dac, Dep_module_expr **eq_mod, 
                    uint and_level, Item_bool_func *cond,
                    Item *left, Item *right);
static 
Dep_module_expr *merge_eq_mods(Dep_module_expr *start, 
                                 Dep_module_expr *new_fields, 
                                 Dep_module_expr *end, uint and_level);
static void mark_as_eliminated(JOIN *join, TABLE_LIST *tbl,
                               Json_writer_array* trace_eliminate_tables);
static 
void add_module_expr(Dep_analysis_context *dac, Dep_module_expr **eq_mod,
                     uint and_level, Dep_value_field *field_val, Item *right,
                     List<Dep_value_field>* mult_equal_fields);


/*****************************************************************************/

/*
  Perform table elimination

  SYNOPSIS
    eliminate_tables()
      join                   Join to work on

  DESCRIPTION
    This is the entry point for table elimination. Grep for MODULE INTERFACE
    section in this file for calling convention.

    The idea behind table elimination is that if we have an outer join:
   
      SELECT * FROM t1 LEFT JOIN 
        (t2 JOIN t3) ON t2.primary_key=t1.col AND 
                        t3.primary_key=t2.col
    such that

    1. columns of the inner tables are not used anywhere ouside the outer
       join (not in WHERE, not in GROUP/ORDER BY clause, not in select list 
       etc etc), and
    2. inner side of the outer join is guaranteed to produce at most one
       record combination for each record combination of outer tables.
    
    then the inner side of the outer join can be removed from the query.
    This is because it will always produce one matching record (either a
    real match or a NULL-complemented record combination), and since there
    are no references to columns of the inner tables anywhere, it doesn't
    matter which record combination it was.

    This function primary handles checking #1. It collects a bitmap of
    tables that are not used in select list/GROUP BY/ORDER BY/HAVING/etc and
    thus can possibly be eliminated.

    After this, if #1 is met, the function calls eliminate_tables_for_list()
    that checks #2.
  
  SIDE EFFECTS
    See the OVERVIEW section at the top of this file.

*/

void eliminate_tables(JOIN *join)
{
  THD* thd= join->thd;
  Item *item;
  table_map used_tables;
  DBUG_ENTER("eliminate_tables");
  
  DBUG_ASSERT(join->eliminated_tables == 0);

  /* If there are no outer joins, we have nothing to eliminate: */
  if (!join->outer_join)
    DBUG_VOID_RETURN;

  if (!optimizer_flag(thd, OPTIMIZER_SWITCH_TABLE_ELIMINATION))
    DBUG_VOID_RETURN; /* purecov: inspected */

  Json_writer_object trace_wrapper(thd);

  /* Find the tables that are referred to from WHERE/HAVING */
  used_tables= (join->conds?  join->conds->used_tables() : 0) | 
               (join->having? join->having->used_tables() : 0);

  /*
    For "INSERT ... SELECT ... ON DUPLICATE KEY UPDATE column = val"
    we should also take into account tables mentioned in "val".
  */
  if (join->thd->lex->sql_command == SQLCOM_INSERT_SELECT &&
      join->select_lex == thd->lex->first_select_lex())
  {
    List_iterator<Item> val_it(thd->lex->value_list);
    while ((item= val_it++))
    {
      DBUG_ASSERT(item->fixed());
      used_tables |= item->used_tables();
    }
  }
  
  /* Add tables referred to from the select list */
  List_iterator<Item> it(join->fields_list);
  while ((item= it++))
    used_tables |= item->used_tables();

  {
    /*
      Table function JSON_TABLE() can have references to other tables. Do not
      eliminate the tables that JSON_TABLE() refers to.
      Note: the JSON_TABLE itself cannot be eliminated as it doesn't
            have unique keys.
    */
    List_iterator<TABLE_LIST> it(join->select_lex->leaf_tables);
    TABLE_LIST *tbl;
    while ((tbl= it++))
    {
      if (tbl->table_function)
        used_tables|= tbl->table_function->used_tables();
    }
  }
 
  /* Add tables referred to from ORDER BY and GROUP BY lists */
  ORDER *all_lists[]= { join->order, join->group_list};
  for (int i=0; i < 2; i++)
  {
    for (ORDER *cur_list= all_lists[i]; cur_list; cur_list= cur_list->next)
      used_tables |= (*(cur_list->item))->used_tables();
  }
  
  if (join->select_lex == thd->lex->first_select_lex())
  {

    /* Multi-table UPDATE: don't eliminate tables referred from SET statement */
    if (thd->lex->sql_command == SQLCOM_UPDATE_MULTI)
    {
      /* Multi-table UPDATE and DELETE: don't eliminate the tables we modify: */
      used_tables |= thd->table_map_for_update;
      List_iterator<Item> it2(thd->lex->value_list);
      while ((item= it2++))
        used_tables |= item->used_tables();
    }

    if (thd->lex->sql_command == SQLCOM_DELETE_MULTI)
    {
      TABLE_LIST *tbl;
      for (tbl= (TABLE_LIST*)thd->lex->auxiliary_table_list.first;
           tbl; tbl= tbl->next_local)
      {
        used_tables |= tbl->table->map;
      }
    }
  }

  table_map all_tables= join->all_tables_map();
  Json_writer_array trace_eliminated_tables(thd,"eliminated_tables");
  if (all_tables & ~used_tables)
  {
    /* There are some tables that we probably could eliminate. Try it. */
    eliminate_tables_for_list(join, join->join_list, all_tables, NULL,
                              used_tables, &trace_eliminated_tables);
  }
  DBUG_VOID_RETURN;
}


/*
  Perform table elimination in a given join list

  SYNOPSIS
    eliminate_tables_for_list()
      join                    The join we're working on
      join_list               Join list to eliminate tables from (and if
                              on_expr !=NULL, then try eliminating join_list
                              itself)
      list_tables             Bitmap of tables embedded in the join_list.
      on_expr                 ON expression, if the join list is the inner side
                              of an outer join.
                              NULL means it's not an outer join but rather a
                              top-level join list.
      tables_used_elsewhere   Bitmap of tables that are referred to from
                              somewhere outside of the join list (e.g.
                              select list, HAVING, other ON expressions, etc).

  DESCRIPTION
    Perform table elimination in a given join list:
    - First, walk through join list members and try doing table elimination for
      them.
    - Then, if the join list itself is an inner side of outer join
      (on_expr!=NULL), then try to eliminate the entire join list.

    See "HANDLING MULTIPLE NESTED OUTER JOINS" section at the top of this file
    for more detailed description and justification.
    
  RETURN
    TRUE   The entire join list eliminated
    FALSE  Join list wasn't eliminated (but some of its child outer joins 
           possibly were)
*/

static bool
eliminate_tables_for_list(JOIN *join, List<TABLE_LIST> *join_list,
                          table_map list_tables, Item *on_expr,
                          table_map tables_used_elsewhere,
                          Json_writer_array *trace_eliminate_tables)
{
  TABLE_LIST *tbl;
  List_iterator<TABLE_LIST> it(*join_list);
  table_map tables_used_on_left= 0;
  bool all_eliminated= TRUE;

  while ((tbl= it++))
  {
    if (tbl->on_expr)
    {
      table_map outside_used_tables= tables_used_elsewhere | 
                                     tables_used_on_left;
      if (on_expr)
        outside_used_tables |= on_expr->used_tables();
      if (tbl->nested_join)
      {
        /* This is  "... LEFT JOIN (join_nest) ON cond" */
        if (eliminate_tables_for_list(join,
                                      &tbl->nested_join->join_list, 
                                      tbl->nested_join->used_tables, 
                                      tbl->on_expr,
                                      outside_used_tables,
                                      trace_eliminate_tables))
        {
          mark_as_eliminated(join, tbl, trace_eliminate_tables);
        }
        else
          all_eliminated= FALSE;
      }
      else
      {
        /* This is  "... LEFT JOIN tbl ON cond" */
        if (!(tbl->table->map & outside_used_tables) &&
            check_func_dependency(join, tbl->table->map, NULL, tbl, 
                                  tbl->on_expr))
        {
          mark_as_eliminated(join, tbl, trace_eliminate_tables);
        }
        else
          all_eliminated= FALSE;
      }
      tables_used_on_left |= tbl->on_expr->used_tables();
    }
    else
    {
      DBUG_ASSERT(!tbl->nested_join || tbl->sj_on_expr);
      //psergey-todo: is the following really correct or we'll need to descend
      //down all ON clauses: ? 
      if (tbl->sj_on_expr)
        tables_used_on_left |= tbl->sj_on_expr->used_tables();
    }
  }

  /* Try eliminating the nest we're called for */
  if (all_eliminated && on_expr && !(list_tables & tables_used_elsewhere))
  {
    it.rewind();
    return check_func_dependency(join, list_tables & ~join->eliminated_tables,
                                 &it, NULL, on_expr);
  }
  return FALSE; /* not eliminated */
}


/*
  Check if given condition makes given set of tables functionally dependent

  SYNOPSIS
    check_func_dependency()
      join         Join we're processing
      dep_tables   Tables that we check to be functionally dependent (on
                   everything else)
      it           Iterator that enumerates these tables, or NULL if we're 
                   checking one single table and it is specified in oj_tbl
                   parameter.
      oj_tbl       NULL, or one single table that we're checking
      cond         Condition to use to prove functional dependency

  DESCRIPTION
    Check if we can use given condition to infer that the set of given tables
    is functionally dependent on everything else.

  RETURN 
    TRUE  - Yes, functionally dependent
    FALSE - No, or error
*/

static
bool check_func_dependency(JOIN *join,
                           table_map dep_tables,
                           List_iterator<TABLE_LIST> *it, 
                           TABLE_LIST *oj_tbl,
                           Item* cond)
{
  Dep_analysis_context dac;
  
  /* 
    Pre-alloc some Dep_module_expr structures. We don't need this to be
    guaranteed upper bound.
  */
  dac.n_equality_mods_alloced= 
    join->thd->lex->current_select->max_equal_elems +
    (join->thd->lex->current_select->cond_count+1)*2 +
    join->thd->lex->current_select->between_count;

  bzero(dac.table_deps, sizeof(dac.table_deps));
  if (!(dac.equality_mods= new Dep_module_expr[dac.n_equality_mods_alloced]))
    return FALSE; /* purecov: inspected */

  Dep_module_expr* last_eq_mod= dac.equality_mods;
  
  /* Create Dep_value_table objects for all tables we're trying to eliminate */
  if (oj_tbl)
  {
    if (!dac.create_table_value(oj_tbl))
      return FALSE; /* purecov: inspected */
  }
  else
  {
    TABLE_LIST *tbl; 
    while ((tbl= (*it)++))
    {
      if (tbl->table && (tbl->table->map & dep_tables))
      {
        if (!dac.create_table_value(tbl))
          return FALSE; /* purecov: inspected */
      }
    }
  }
  dac.usable_tables= dep_tables;

  /*
    Analyze the the ON expression and create Dep_module_expr objects and
      Dep_value_field objects for the used fields.
  */
  uint and_level=0;
  build_eq_mods_for_cond(join->thd, &dac, &last_eq_mod, &and_level, cond);
  if (!(dac.n_equality_mods= (uint)(last_eq_mod - dac.equality_mods)))
    return FALSE;  /* No useful conditions */

  List<Dep_module> bound_modules;

  if (!(dac.outer_join_dep= new Dep_module_goal(my_count_bits(dep_tables))) ||
      dac.setup_equality_modules_deps(&bound_modules))
  {
    return FALSE; /* OOM, default to non-dependent */ /* purecov: inspected */
  }
  
  DBUG_EXECUTE("test", dac.dbug_print_deps(); );
  
  return dac.run_wave(&bound_modules);
}


/*
  Running wave functional dependency check algorithm

  SYNOPSIS
   Dep_analysis_context::run_wave()
     new_bound_modules  List of bound modules to start the running wave from. 
                        The list is destroyed during execution
  
  DESCRIPTION
    This function uses running wave algorithm to check if the join nest is
    functionally-dependent. 
    We start from provided list of bound modules, and then run the wave across 
    dependency edges, trying the reach the Dep_module_goal module. If we manage
    to reach it, then the join nest is functionally-dependent, otherwise it is
    not.

  RETURN 
    TRUE   Yes, functionally dependent
    FALSE  No.
*/

bool Dep_analysis_context::run_wave(List<Dep_module> *new_bound_modules)
{
  List<Dep_value> new_bound_values;
  
  Dep_value *value;
  Dep_module *module;

  while (!new_bound_modules->is_empty())
  {
    /*
      The "wave" is in new_bound_modules list. Iterate over values that can be
      reached from these modules but are not yet bound, and collect the next
      wave generation in new_bound_values list.
    */
    List_iterator<Dep_module> modules_it(*new_bound_modules);
    while ((module= modules_it++))
    {
      char iter_buf[Dep_module::iterator_size + ALIGN_MAX_UNIT];
      Dep_module::Iterator iter;
      iter= module->init_unbound_values_iter(iter_buf);
      while ((value= module->get_next_unbound_value(this, iter)))
      {
        if (!value->is_bound())
        {
          value->make_bound();
          new_bound_values.push_back(value);
        }
      }
    }
    new_bound_modules->empty();
    
    /*
      Now walk over list of values we've just found to be bound and check which
      unbound modules can be reached from them. If there are some modules that
      became bound, collect them in new_bound_modules list.
    */
    List_iterator<Dep_value> value_it(new_bound_values);
    while ((value= value_it++))
    {
      char iter_buf[Dep_value::iterator_size + ALIGN_MAX_UNIT];
      Dep_value::Iterator iter;
      iter= value->init_unbound_modules_iter(iter_buf);
      while ((module= value->get_next_unbound_module(this, iter)))
      {
        module->touch();
        if (!module->is_applicable())
          continue;
        if (module->is_final())
          return TRUE; /* Functionally dependent */
        module->make_bound();
        new_bound_modules->push_back(module);
      }
    }
    new_bound_values.empty();
  }
  return FALSE;
}


/*
  This is used to analyze expressions in "tbl.col=expr" dependencies so
  that we can figure out which fields the expression depends on.
*/

class Field_dependency_recorder : public Field_enumerator
{
public:
  Field_dependency_recorder(Dep_analysis_context *ctx_arg): ctx(ctx_arg)
  {}
  
  void visit_field(Item_field *item) override
  {
    Field *field= item->field;
    Dep_value_table *tbl_dep;
    if ((tbl_dep= ctx->table_deps[field->table->tablenr]))
    {
      for (Dep_value_field *field_dep= tbl_dep->fields; field_dep; 
           field_dep= field_dep->next_table_field)
      {
        if (field->field_index == field_dep->field->field_index)
        {
          uint offs= field_dep->bitmap_offset + expr_offset;
          if (!bitmap_is_set(&ctx->expr_deps, offs))
            ctx->equality_mods[expr_offset].unbound_args++;
          bitmap_set_bit(&ctx->expr_deps, offs);
          return;
        }
      }
      /* 
        We got here if didn't find this field. It's not a part of 
        a unique key, and/or there is no field=expr element for it.
        Bump the dependency anyway, this will signal that this dependency
        cannot be satisfied.
      */
      ctx->equality_mods[expr_offset].unbound_args++;
    }
    else
      visited_other_tables= TRUE;
  }

  Dep_analysis_context *ctx;
  /* Offset of the expression we're processing in the dependency bitmap */
  uint expr_offset;

  bool visited_other_tables;
};




/*
  Setup inbound dependency relationships for tbl.col=expr equalities
 
  SYNOPSIS
    setup_equality_modules_deps()
      bound_deps_list  Put here modules that were found not to depend on 
                       any non-bound columns.

  DESCRIPTION
    Setup inbound dependency relationships for tbl.col=expr equalities:
      - allocate a bitmap where we store such dependencies
      - for each "tbl.col=expr" equality, analyze the expr part and find out
        which fields it refers to and set appropriate dependencies.
    
  RETURN
    FALSE  OK
    TRUE   Out of memory
*/

bool Dep_analysis_context::setup_equality_modules_deps(List<Dep_module> 
                                                       *bound_modules)
{
  THD *thd= current_thd;
  DBUG_ENTER("setup_equality_modules_deps");
 
  /*
    Count Dep_value_field objects and assign each of them a unique 
    bitmap_offset value.
  */
  uint offset= 0;
  for (Dep_value_table **tbl_dep= table_deps; 
       tbl_dep < table_deps + MAX_TABLES;
       tbl_dep++)
  {
    if (*tbl_dep)
    {
      for (Dep_value_field *field_dep= (*tbl_dep)->fields;
           field_dep;
           field_dep= field_dep->next_table_field)
      {
        field_dep->bitmap_offset= offset;
        offset += n_equality_mods;
      }
    }
  }
 
  void *buf;
  if (!(buf= thd->alloc(bitmap_buffer_size(offset))) ||
      my_bitmap_init(&expr_deps, (my_bitmap_map*)buf, offset))
  {
    DBUG_RETURN(TRUE); /* purecov: inspected */
  }
  bitmap_clear_all(&expr_deps);

  /* 
    Analyze all "field=expr" dependencies, and have expr_deps encode
    dependencies of expressions from fields.

    Also collect a linked list of equalities that are bound.
  */
  Field_dependency_recorder deps_recorder(this);
  for (Dep_module_expr *eq_mod= equality_mods; 
       eq_mod < equality_mods + n_equality_mods;
       eq_mod++)
  {
    deps_recorder.expr_offset= (uint)(eq_mod - equality_mods);
    deps_recorder.visited_other_tables= FALSE;
    eq_mod->unbound_args= 0;
    
    if (eq_mod->field)
    {
      /* Regular tbl.col=expr(tblX1.col1, tblY1.col2, ...) */
      eq_mod->expr->walk(&Item::enumerate_field_refs_processor, FALSE, 
                               &deps_recorder);
    }
    else 
    {
      /* It's a multi-equality */
      eq_mod->unbound_args= !MY_TEST(eq_mod->expr);
      List_iterator<Dep_value_field> it(*eq_mod->mult_equal_fields);
      Dep_value_field* field_val;
      while ((field_val= it++))
      {
        uint offs= (uint)(field_val->bitmap_offset + eq_mod - equality_mods);
        bitmap_set_bit(&expr_deps, offs);
      }
    }

    if (!eq_mod->unbound_args)
      bound_modules->push_back(eq_mod, thd->mem_root);
  }

  DBUG_RETURN(FALSE);
}


/*
  Ordering that we're using whenever we need to maintain a no-duplicates list
  of field value objects.
*/

static 
int compare_field_values(Dep_value_field *a, Dep_value_field *b, void *unused)
{
  uint a_ratio= a->field->table->tablenr*MAX_FIELDS +
                a->field->field_index;

  uint b_ratio= b->field->table->tablenr*MAX_FIELDS +
                b->field->field_index;
  return (a_ratio < b_ratio)? 1 : ((a_ratio == b_ratio)? 0 : -1);
}


/*
  Produce Dep_module_expr elements for given condition.

  SYNOPSIS
    build_eq_mods_for_cond()
      ctx              Table elimination context
      eq_mod    INOUT  Put produced equality conditions here
      and_level INOUT  AND-level (like in add_key_fields)
      cond             Condition to process

  DESCRIPTION
    Analyze the given condition and produce an array of Dep_module_expr 
    dependencies from it. The idea of analysis is as follows:
    There are useful equalities that have form 
        
        eliminable_tbl.field = expr      (denote as useful_equality)

    The condition is composed of useful equalities and other conditions that
    are combined together with AND and OR operators. We process the condition
    in recursive fashion according to these basic rules:

      useful_equality1 AND useful_equality2 -> make array of two 
                                               Dep_module_expr objects

      useful_equality AND other_cond -> discard other_cond
      
      useful_equality OR other_cond -> discard everything
      
      useful_equality1 OR useful_equality2 -> check if both sides of OR are the
                                              same equality. If yes, that's the
                                              result, otherwise discard 
                                              everything.

    The rules are used to map the condition into an array Dep_module_expr
    elements. The array will specify functional dependencies that logically 
    follow from the condition.

  SEE ALSO
    This function is modeled after add_key_fields()
*/

static 
void build_eq_mods_for_cond(THD *thd, Dep_analysis_context *ctx,
                            Dep_module_expr **eq_mod,
                            uint *and_level, Item *cond)
{
  if (cond->type() == Item_func::COND_ITEM)
  {
    List_iterator_fast<Item> li(*((Item_cond*) cond)->argument_list());
    size_t orig_offset= *eq_mod - ctx->equality_mods;
    
    /* AND/OR */
    if (((Item_cond*) cond)->functype() == Item_func::COND_AND_FUNC)
    {
      Item *item;
      while ((item=li++))
        build_eq_mods_for_cond(thd, ctx, eq_mod, and_level, item);

      for (Dep_module_expr *mod_exp= ctx->equality_mods + orig_offset;
           mod_exp != *eq_mod ; mod_exp++)
      {
        mod_exp->level= *and_level;
      }
    }
    else
    {
      Item *item;
      (*and_level)++;
      build_eq_mods_for_cond(thd, ctx, eq_mod, and_level, li++);
      while ((item=li++))
      {
        Dep_module_expr *start_key_fields= *eq_mod;
        (*and_level)++;
        build_eq_mods_for_cond(thd, ctx, eq_mod, and_level, item);
        *eq_mod= merge_eq_mods(ctx->equality_mods + orig_offset, 
                               start_key_fields, *eq_mod,
                               ++(*and_level));
      }
    }
    return;
  }

  if (cond->type() != Item::FUNC_ITEM)
    return;

  Item_func *cond_func= (Item_func*) cond;
  Item **args= cond_func->arguments();

  switch (cond_func->functype()) {
  case Item_func::BETWEEN:
  {
    Item *fld;
    Item_func_between *func= (Item_func_between *) cond_func;
    if (!func->negated &&
        (fld= args[0]->real_item())->type() == Item::FIELD_ITEM &&
        args[1]->eq(args[2], ((Item_field*)fld)->field->binary()))
    {
      check_equality(ctx, eq_mod, *and_level, func, args[0], args[1]);
      check_equality(ctx, eq_mod, *and_level, func, args[1], args[0]);
    }
    break;
  }
  case Item_func::EQ_FUNC:
  case Item_func::EQUAL_FUNC:
  {
    Item_bool_rowready_func2 *func= (Item_bool_rowready_func2*) cond_func;
    check_equality(ctx, eq_mod, *and_level, func, args[0], args[1]);
    check_equality(ctx, eq_mod, *and_level, func, args[1], args[0]);
    break;
  }
  case Item_func::ISNULL_FUNC:
  {
    Item *tmp=new (thd->mem_root) Item_null(thd);
    if (tmp)
      check_equality(ctx, eq_mod, *and_level,
                     (Item_func_isnull*) cond_func, args[0], tmp);
    break;
  }
  case Item_func::MULT_EQUAL_FUNC:
  {
    /*
      The condition is a 

        tbl1.field1 = tbl2.field2 = tbl3.field3 [= const_expr]

      multiple-equality. Do two things:
       - Collect List<Dep_value_field> of tblX.colY where tblX is one of the
         tables we're trying to eliminate.
       - remember if there was a bound value, either const_expr or tblY.colZ
         where tblY is not a table that we're trying to eliminate.
      Store all collected information in a Dep_module_expr object.
    */
    Item_equal *item_equal= (Item_equal*)cond;
    List<Dep_value_field> *fvl;
    if (!(fvl= new List<Dep_value_field>))
      break; /* purecov: inspected */

    Item_equal_fields_iterator it(*item_equal);
    Item *item;
    Item *bound_item= item_equal->get_const();
    while ((item= it++))
    {
      Field *equal_field= it.get_curr_field();
      if ((item->used_tables() & ctx->usable_tables))
      {
        Dep_value_field *field_val;
        if ((field_val= ctx->get_field_value(equal_field)))
          fvl->push_back(field_val, thd->mem_root);
      }
      else
      {
        if (!bound_item)
          bound_item= item;
      }
    }
    /* 
      Multiple equality is only useful if it includes at least one field from
      the table that we could potentially eliminate:
    */
    if (fvl->elements)
    {
      
      bubble_sort<Dep_value_field>(fvl, compare_field_values, NULL);
      add_module_expr(ctx, eq_mod, *and_level, NULL, bound_item, fvl);
    }
    break;
  }
  default:
    break;
  }
}


/*
  Perform an OR operation on two (adjacent) Dep_module_expr arrays.

  SYNOPSIS
     merge_eq_mods()
       start        Start of left OR-part
       new_fields   Start of right OR-part
       end          End of right OR-part
       and_level    AND-level (like in add_key_fields)

  DESCRIPTION
  This function is invoked for two adjacent arrays of Dep_module_expr elements:

                      $LEFT_PART             $RIGHT_PART
             +-----------------------+-----------------------+
            start                new_fields                 end
         
  The goal is to produce an array which would correspond to the combined 
  
    $LEFT_PART OR $RIGHT_PART
  
  condition. This is achieved as follows: First, we apply distributive law:
  
    (fdep_A_1 AND fdep_A_2 AND ...)  OR  (fdep_B_1 AND fdep_B_2 AND ...) =

     = AND_ij (fdep_A_[i] OR fdep_B_[j])
  
  Then we walk over the obtained "fdep_A_[i] OR fdep_B_[j]" pairs, and 
   - Discard those that that have left and right part referring to different
     columns. We can't infer anything useful from "col1=expr1 OR col2=expr2".
   - When left and right parts refer to the same column,  we check if they are 
     essentially the same. 
     = If they are the same, we keep one copy 
       "t.col=expr OR t.col=expr"  -> "t.col=expr 
     = if they are different , then we discard both
      "t.col=expr1 OR t.col=expr2" -> (nothing useful)

  (no per-table or for-index FUNC_DEPS exist yet at this phase).

  See also merge_key_fields().

  RETURN 
    End of the result array
*/

static 
Dep_module_expr *merge_eq_mods(Dep_module_expr *start, 
                               Dep_module_expr *new_fields,
                               Dep_module_expr *end, uint and_level)
{
  if (start == new_fields)
    return start;  /*  (nothing) OR (...) -> (nothing) */
  if (new_fields == end)
    return start;  /*  (...) OR (nothing) -> (nothing) */

  Dep_module_expr *first_free= new_fields;

  for (; new_fields != end ; new_fields++)
  {
    for (Dep_module_expr *old=start ; old != first_free ; old++)
    {
      if (old->field == new_fields->field)
      {
        if (!old->field)
        {
          /*
            OR-ing two multiple equalities. We must compute an intersection of
            used fields, and check the constants according to these rules:

              a=b=c=d  OR a=c=e=f   ->  a=c  (compute intersection)
              a=const1 OR a=b       ->  (nothing)
              a=const1 OR a=const1  ->  a=const1 
              a=const1 OR a=const2  ->  (nothing)
            
            If we're performing an OR operation over multiple equalities, e.g.

              (a=b=c AND p=q) OR (a=b AND v=z)
            
            then we'll need to try combining each equality with each. ANDed
            equalities are guaranteed to be disjoint, so we'll only get one
            hit.
          */
          Field *eq_field= old->mult_equal_fields->head()->field;
          if (old->expr && new_fields->expr &&
              old->expr->eq_by_collation(new_fields->expr, eq_field->binary(),
                                         eq_field->charset()))
          {
            /* Ok, keep */
          }
          else
          {
            /* no single constant/bound item. */
            old->expr= NULL;
          }
           
          List <Dep_value_field> *fv;
          if (!(fv= new List<Dep_value_field>))
            break; /* purecov: inspected */

          List_iterator<Dep_value_field> it1(*old->mult_equal_fields);
          List_iterator<Dep_value_field> it2(*new_fields->mult_equal_fields);
          Dep_value_field *lfield= it1++;
          Dep_value_field *rfield= it2++;
          /* Intersect two ordered lists */
          while (lfield && rfield)
          {
            if (lfield == rfield)
            {
              fv->push_back(lfield);
              lfield=it1++;
              rfield=it2++;
            }
            else
            {
              if (compare_field_values(lfield, rfield, NULL) < 0)
                lfield= it1++;
              else
                rfield= it2++;
            }
          }

          if (fv->elements + MY_TEST(old->expr) > 1)
          {
            old->mult_equal_fields= fv;
            old->level= and_level;
          }
        }
        else if (!new_fields->expr->const_item())
        {
          /*
            If the value matches, we can use the key reference.
            If not, we keep it until we have examined all new values
          */
          if (old->expr->eq(new_fields->expr, 
                            old->field->field->binary()))
          {
            old->level= and_level;
          }
        }
        else if (old->expr->eq_by_collation(new_fields->expr,
                                            old->field->field->binary(),
                                            old->field->field->charset()))
        {
          old->level= and_level;
        }
        else
        {
          /* The expressions are different. */
          if (old == --first_free)                // If last item
            break;
          *old= *first_free;                        // Remove old value
          old--;                                // Retry this value
        }
      }
    }
  }

  /* 
    Ok, the results are within the [start, first_free) range, and the useful
    elements have level==and_level. Now, remove all unusable elements:
  */
  for (Dep_module_expr *old=start ; old != first_free ;)
  {
    if (old->level != and_level)
    {                                                // Not used in all levels
      if (old == --first_free)
        break;
      *old= *first_free;                        // Remove old value
      continue;
    }
    old++;
  }
  return first_free;
}


/*
  Add an Dep_module_expr element for left=right condition

  SYNOPSIS
    check_equality()
      fda               Table elimination context
      eq_mod     INOUT  Store created Dep_module_expr here and increment ptr if
                        you do so
      and_level         AND-level (like in add_key_fields)
      cond              Condition we've inferred the left=right equality from.
      left              Left expression
      right             Right expression
      usable_tables     Create Dep_module_expr only if Left_expression's table 
                        belongs to this set.

  DESCRIPTION 
    Check if the passed left=right equality is such that 
     - 'left' is an Item_field referring to a field in a table we're checking
       to be functionally depdendent,
     - the equality allows to conclude that 'left' expression is functionally 
       dependent on the 'right',
    and if so, create an Dep_module_expr object.
*/

static 
void check_equality(Dep_analysis_context *ctx, Dep_module_expr **eq_mod,
                    uint and_level, Item_bool_func *cond,
                    Item *left, Item *right)
{
  if ((left->used_tables() & ctx->usable_tables) &&
      !(right->used_tables() & RAND_TABLE_BIT) &&
      left->real_item()->type() == Item::FIELD_ITEM)
  {
    Field *field= ((Item_field*)left->real_item())->field;
    if (field->can_optimize_outer_join_table_elimination(cond, right) !=
        Data_type_compatibility::OK)
      return;
    Dep_value_field *field_val;
    if ((field_val= ctx->get_field_value(field)))
      add_module_expr(ctx, eq_mod, and_level, field_val, right, NULL);
  }
}


/* 
  Add a Dep_module_expr object with the specified parameters. 
  
  DESCRIPTION
    Add a Dep_module_expr object with the specified parameters. Re-allocate
    the ctx->equality_mods array if it has no space left.
*/

static 
void add_module_expr(Dep_analysis_context *ctx, Dep_module_expr **eq_mod,
                     uint and_level, Dep_value_field *field_val, 
                     Item *right, List<Dep_value_field>* mult_equal_fields)
{
  if (*eq_mod == ctx->equality_mods + ctx->n_equality_mods_alloced)
  {
    /* 
      We've filled the entire equality_mods array. Replace it with a bigger
      one. We do it somewhat inefficiently but it doesn't matter.
    */
    /* purecov: begin inspected */
    Dep_module_expr *new_arr;
    if (!(new_arr= new Dep_module_expr[ctx->n_equality_mods_alloced *2]))
      return;
    ctx->n_equality_mods_alloced *= 2;
    for (int i= 0; i < *eq_mod - ctx->equality_mods; i++)
      new_arr[i]= ctx->equality_mods[i];

    ctx->equality_mods= new_arr;
    *eq_mod= new_arr + (*eq_mod - ctx->equality_mods);
    /* purecov: end */
  }

  (*eq_mod)->field= field_val;
  (*eq_mod)->expr= right;
  (*eq_mod)->level= and_level;
  (*eq_mod)->mult_equal_fields= mult_equal_fields;
  (*eq_mod)++;
}


/*
  Create a Dep_value_table object for the given table

  SYNOPSIS
    Dep_analysis_context::create_table_value()
      table  Table to create object for

  DESCRIPTION
    Create a Dep_value_table object for the given table. Also create
    Dep_module_key objects for all unique keys in the table.
    Create a unique pseudo-key if this table is derived and has
    a GROUP BY expression.

  RETURN
    Created table value object
    NULL if out of memory
*/

Dep_value_table *
Dep_analysis_context::create_table_value(TABLE_LIST *table_list)
{
  Dep_value_table *tbl_dep;
  if (!(tbl_dep= new Dep_value_table(table_list->table)))
    return NULL; /* purecov: inspected */

  Dep_module_key **key_list= &(tbl_dep->keys);
  /* Add dependencies for unique keys */
  for (uint i= 0; i < table_list->table->s->keys; i++)
  {
    KEY *key= table_list->table->key_info + i;
    if (key->flags & HA_NOSAME)
    {
      Dep_module_key *key_dep;
      if (!(key_dep= new Dep_module_key(tbl_dep, i,
                                        key->user_defined_key_parts)))
        return NULL;
      *key_list= key_dep;
      key_list= &(key_dep->next_table_key);
    }
  }

  create_unique_pseudo_key_if_needed(table_list, tbl_dep);
  return table_deps[table_list->table->tablenr]= tbl_dep;
}


/*
  @brief
    Check if we can create a unique pseudo-key for the passed table.
    If we can, create a dependency for it

  @detail
    Currently, pseudo-key is created for the list of GROUP BY columns.

    TODO: also it can be created if the query uses
     - SELECT DISTINCT
     - UNION DISTINCT (not UNION ALL)
*/

void Dep_analysis_context::create_unique_pseudo_key_if_needed(
    TABLE_LIST *table_list, Dep_value_table *tbl_dep)
{
  auto select_unit= table_list->get_unit();
  SELECT_LEX *first_select= nullptr;
  if (select_unit)
  {
    first_select= select_unit->first_select();

    /*
      Exclude UNION (ALL) queries from consideration by checking
      next_select() == nullptr
    */
    if (unlikely(select_unit->first_select()->next_select()))
      first_select= nullptr;
  }

  /*
    GROUP BY expression is considered as a unique pseudo-key
    for the derived table. Add this pseudo key as a dependency.

    first_select->join is NULL for degenerate derived tables
    which are known to have just one row and so were already materialized
    by the optimizer, check this here
  */
  if (first_select && first_select->join &&
      first_select->group_list.elements > 0)
  {
    auto max_possible_elements= first_select->join->fields_list.elements;
    void *buf;
    MY_BITMAP *exposed_fields= (MY_BITMAP*)
        current_thd->alloc<MY_BITMAP>(1);
    if (!(buf= current_thd->alloc(bitmap_buffer_size(max_possible_elements))) ||
        my_bitmap_init(exposed_fields, (my_bitmap_map*)buf,
                       max_possible_elements))
      // Memory allocation failed
      return;
    bitmap_clear_all(exposed_fields);
    uint exposed_fields_count= 0;

    bool valid= true;
    for (auto cur_group= first_select->group_list.first;
         cur_group;
         cur_group= cur_group->next)
    {
      auto elem= *(cur_group->item);
      /*
        Make sure GROUP BY elements contain only fields
        and no functions or other expressions
      */
      if (elem->type() != Item::FIELD_ITEM)
      {
        valid= false;
        break;
      }
      auto field_no= find_field_in_list(first_select->join->fields_list, elem);
      if (field_no == -1)
      {
        /*
          This GROUP BY element is not present in the select list. This is a
          case like this:
             (SELECT a FROM t1 GROUP by a,b) as TBL
          Here, the combination of (a,b) is unique, but the select doesn't
          include "b". "a" alone is not unique, so TBL doesn't have a unique
          pseudo-key.
        */
        valid= false;
        break;
      }
      bitmap_set_bit(exposed_fields, field_no);
      exposed_fields_count++;
    }
    if (valid)
    {
      Dep_module_pseudo_key *pseudo_key;
      pseudo_key= new Dep_module_pseudo_key(tbl_dep, exposed_fields,
                                            exposed_fields_count);
      tbl_dep->pseudo_key= pseudo_key;
    }
  }
}


/*
  Iterate the list of fields and look for the given field.
  Returns the index of the field if it is found on the list
  and -1 otherwise
*/

int Dep_analysis_context::find_field_in_list(List<Item> &fields_list,
                                             Item *field)
{
  List_iterator<Item> it(fields_list);
  int field_idx= 0;
  while (auto next_field= it++)
  {
    if (next_field->eq(field, false))
      return field_idx;
    field_idx++;
  }
  return -1; /*not found*/
}


/* 
  Get a Dep_value_field object for the given field, creating it if necessary

  SYNOPSIS
   Dep_analysis_context::get_field_value()
      field  Field to create object for
        
  DESCRIPTION
    Get a Dep_value_field object for the given field. First, we search for it 
    in the list of Dep_value_field objects we have already created. If we don't 
    find it, we create a new Dep_value_field and put it into the list of field
    objects we have for the table.

  RETURN
    Created field value object
    NULL if out of memory
*/

Dep_value_field *Dep_analysis_context::get_field_value(Field *field)
{
  TABLE *table= field->table;
  Dep_value_table *tbl_dep= table_deps[table->tablenr];

  /* Try finding the field in field list */
  Dep_value_field **pfield= &(tbl_dep->fields);
  while (*pfield && (*pfield)->field->field_index < field->field_index)
  {
    pfield= &((*pfield)->next_table_field);
  }
  if (*pfield && (*pfield)->field->field_index == field->field_index)
    return *pfield;
  
  /* Create the field and insert it in the list */
  Dep_value_field *new_field= new Dep_value_field(tbl_dep, field);
  new_field->next_table_field= *pfield;
  *pfield= new_field;

  return new_field;
}


/* 
  Iteration over unbound modules that are our dependencies.
  for those we have:
    - dependencies of our fields
    - outer join we're in 
*/
char *Dep_value_table::init_unbound_modules_iter(char *buf)
{
  Module_iter *iter= ALIGN_PTR(my_ptrdiff_t(buf), Module_iter);
  iter->field_dep= fields;
  if (fields)
  {
    fields->init_unbound_modules_iter(iter->buf);
    fields->make_unbound_modules_iter_skip_keys(iter->buf);
  }
  iter->returned_goal= FALSE;
  return (char*)iter;
}


Dep_module* 
Dep_value_table::get_next_unbound_module(Dep_analysis_context *dac,
                                         char *iter)
{
  Module_iter *di= (Module_iter*)iter;
  while (di->field_dep)
  {
    Dep_module *res;
    if ((res= di->field_dep->get_next_unbound_module(dac, di->buf)))
      return res;
    if ((di->field_dep= di->field_dep->next_table_field))
    {
      char *field_iter= ((Module_iter*)iter)->buf;
      di->field_dep->init_unbound_modules_iter(field_iter);
      di->field_dep->make_unbound_modules_iter_skip_keys(field_iter);
    }
  }
  
  if (!di->returned_goal)
  {
    di->returned_goal= TRUE;
    return dac->outer_join_dep;
  }
  return NULL;
}


char *Dep_module_expr::init_unbound_values_iter(char *buf)
{
  Value_iter *iter= ALIGN_PTR(my_ptrdiff_t(buf), Value_iter);
  iter->field= field;
  if (!field)
  {
    new (&iter->it) List_iterator<Dep_value_field>(*mult_equal_fields);
  }
  return (char*)iter;
}


Dep_value* Dep_module_expr::get_next_unbound_value(Dep_analysis_context *dac,
                                                   char *buf)
{
  Dep_value *res;
  if (field)
  {
    res= ((Value_iter*)buf)->field;
    ((Value_iter*)buf)->field= NULL;
    return (!res || res->is_bound())? NULL : res;
  }
  else
  {
    while ((res= ((Value_iter*)buf)->it++))
    {
      if (!res->is_bound())
        return res;
    }
    return NULL;
  }
}


char *Dep_module_key::init_unbound_values_iter(char *buf)
{
  Value_iter *iter= ALIGN_PTR(my_ptrdiff_t(buf), Value_iter);
  iter->table= table;
  return (char*)iter;
}


Dep_value* Dep_module_key::get_next_unbound_value(Dep_analysis_context *dac,
                                                  Dep_module::Iterator iter)
{
  Dep_value* res= ((Value_iter*)iter)->table;
  ((Value_iter*)iter)->table= NULL;
  return res;
}


char *Dep_module_pseudo_key::init_unbound_values_iter(char *buf)
{
  Value_iter *iter= ALIGN_PTR(my_ptrdiff_t(buf), Value_iter);
  iter->table= table;
  return (char *) iter;
}

Dep_value *
Dep_module_pseudo_key::get_next_unbound_value(Dep_analysis_context *dac,
                                                  Dep_module::Iterator iter)
{
  Dep_value *res= ((Value_iter *) iter)->table;
  ((Value_iter *) iter)->table= NULL;
  return res;
}


/*
  Check if column number field_no is covered by the pseudo-key.
*/

bool Dep_module_pseudo_key::covers_field(int field_no)
{
  return bitmap_is_set(exposed_fields_map, field_no) > 0;
}


Dep_value::Iterator Dep_value_field::init_unbound_modules_iter(char *buf)
{
  Module_iter *iter= ALIGN_PTR(my_ptrdiff_t(buf), Module_iter);
  iter->key_dep= table->keys;
  iter->equality_no= 0;
  iter->pseudo_key_dep= table->pseudo_key;
  return (char*)iter;
}


void 
Dep_value_field::make_unbound_modules_iter_skip_keys(Dep_value::Iterator iter)
{
  ((Module_iter*) iter)->key_dep= NULL;
  ((Module_iter*) iter)->pseudo_key_dep= NULL;
}


Dep_module* Dep_value_field::get_next_unbound_module(Dep_analysis_context *dac,
                                                     Dep_value::Iterator iter)
{
  Module_iter *di= (Module_iter*)iter;
  Dep_module_key *key_dep= di->key_dep;
  
  /* 
    First, enumerate all unique keys that are 
    - not yet applicable
    - have this field as a part of them
  */
  while (key_dep && (key_dep->is_applicable() ||
         !field->part_of_key_not_clustered.is_set(key_dep->keyno)))
  {
    key_dep= key_dep->next_table_key;
  }

  if (key_dep)
  {
    di->key_dep= key_dep->next_table_key;
    return key_dep;
  }
  else 
    di->key_dep= NULL;

  Dep_module_pseudo_key *pseudo_key_dep= di->pseudo_key_dep;
  if (pseudo_key_dep && !pseudo_key_dep->is_applicable() &&
      pseudo_key_dep->covers_field(field->field_index))
  {
    di->pseudo_key_dep= NULL;
    return pseudo_key_dep;
  }
  else
    di->pseudo_key_dep= NULL;
  
  /*
    Then walk through [multi]equalities and find those that
     - depend on this field
     - and are not bound yet.
  */
  uint eq_no= di->equality_no;
  while (eq_no < dac->n_equality_mods && 
         (!bitmap_is_set(&dac->expr_deps, bitmap_offset + eq_no) ||
         dac->equality_mods[eq_no].is_applicable()))
  {
    eq_no++;
  }
  
  if (eq_no < dac->n_equality_mods)
  {
    di->equality_no= eq_no+1;
    return &dac->equality_mods[eq_no];
  }
  return NULL;
}


/* 
  Mark one table or the whole join nest as eliminated.
*/

static void mark_as_eliminated(JOIN *join, TABLE_LIST *tbl,
                               Json_writer_array* trace_eliminate_tables)
{
  TABLE *table;
  /*
    NOTE: there are TABLE_LIST object that have
    tbl->table!= NULL && tbl->nested_join!=NULL and
    tbl->table == tbl->nested_join->join_list->element(..)->table
  */
  if (tbl->nested_join)
  {
    TABLE_LIST *child;
    List_iterator<TABLE_LIST> it(tbl->nested_join->join_list);
    while ((child= it++))
      mark_as_eliminated(join, child, trace_eliminate_tables);
  }
  else if ((table= tbl->table))
  {
    JOIN_TAB *tab= tbl->table->reginfo.join_tab;
    if (!(join->const_table_map & tab->table->map))
    {
      DBUG_PRINT("info", ("Eliminated table %s", table->alias.c_ptr()));
      tab->type= JT_CONST;
      tab->table->const_table= 1;
      join->eliminated_tables |= table->map;
      trace_eliminate_tables->add(table->alias.c_ptr_safe());
      join->const_table_map|= table->map;
      set_position(join, join->const_tables++, tab, (KEYUSE*)0);
    }
  }

  if (tbl->on_expr)
    tbl->on_expr->walk(&Item::mark_as_eliminated_processor, FALSE, NULL);
}

#ifndef DBUG_OFF
/* purecov: begin inspected */
void Dep_analysis_context::dbug_print_deps()
{
  DBUG_ENTER("dbug_print_deps");
  DBUG_LOCK_FILE;
  
  fprintf(DBUG_FILE,"deps {\n");
  
  /* Start with printing equalities */
  for (Dep_module_expr *eq_mod= equality_mods; 
       eq_mod != equality_mods + n_equality_mods; eq_mod++)
  {
    char buf[128];
    String str(buf, sizeof(buf), &my_charset_bin);
    str.length(0);
    eq_mod->expr->print(&str, QT_ORDINARY);
    if (eq_mod->field)
    {
      fprintf(DBUG_FILE, "  equality%ld: %s -> %s.%s\n", 
              (long)(eq_mod - equality_mods),
              str.c_ptr(),
              eq_mod->field->table->table->alias.c_ptr(),
              eq_mod->field->field->field_name.str);
    }
    else
    {
      fprintf(DBUG_FILE, "  equality%ld: multi-equality", 
              (long)(eq_mod - equality_mods));
    }
  }
  fprintf(DBUG_FILE,"\n");

  /* Then tables and their fields */
  for (uint i=0; i < MAX_TABLES; i++)
  {
    Dep_value_table *table_dep;
    if ((table_dep= table_deps[i]))
    {
      /* Print table */
      fprintf(DBUG_FILE, "  table %s\n", table_dep->table->alias.c_ptr());
      /* Print fields */
      for (Dep_value_field *field_dep= table_dep->fields; field_dep; 
           field_dep= field_dep->next_table_field)
      {
        fprintf(DBUG_FILE, "    field %s.%s ->",
                table_dep->table->alias.c_ptr(),
                field_dep->field->field_name.str);
        uint ofs= field_dep->bitmap_offset;
        for (uint bit= ofs; bit < ofs + n_equality_mods; bit++)
        {
          if (bitmap_is_set(&expr_deps, bit))
            fprintf(DBUG_FILE, " equality%d ", bit - ofs);
        }
        fprintf(DBUG_FILE, "\n");
      }
    }
  }
  fprintf(DBUG_FILE,"\n}\n");
  DBUG_UNLOCK_FILE;
  DBUG_VOID_RETURN;
}
/* purecov: end */

#endif 
/**
  @} (end of group Table_Elimination)
*/

