/*****************************************************************************

Copyright (c) 1994, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2012, Facebook Inc.
Copyright (c) 2018, 2023, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/********************************************************************//**
@file page/page0cur.cc
The page cursor

Created 10/4/1994 Heikki Tuuri
*************************************************************************/

#include "page0cur.h"
#include "page0zip.h"
#include "btr0btr.h"
#include "mtr0log.h"
#include "log0recv.h"
#include "rem0cmp.h"
#include "gis0rtree.h"
#ifdef UNIV_DEBUG
# include "trx0roll.h"
#endif

/** Get the pad character code point for a type.
@param type
@return pad character code point
@retval ULINT_UNDEFINED if no padding is specified */
static ulint cmp_get_pad_char(const dtype_t &type) noexcept
{
  switch (type.mtype) {
  default:
    break;
  case DATA_FIXBINARY:
  case DATA_BINARY:
    if (dtype_get_charset_coll(type.prtype) ==
        DATA_MYSQL_BINARY_CHARSET_COLL)
      /* Starting from 5.0.18, we do not pad VARBINARY or BINARY columns. */
      break;
    /* Fall through */
  case DATA_CHAR:
  case DATA_VARCHAR:
  case DATA_MYSQL:
  case DATA_VARMYSQL:
    /* Space is the padding character for all char and binary
    strings, and starting from 5.0.3, also for TEXT strings. */
    return 0x20;
  case DATA_BLOB:
    if (!(type.prtype & DATA_BINARY_TYPE))
      return 0x20;
  }

  /* No padding specified */
  return ULINT_UNDEFINED;
}

/** Compare a data tuple to a physical record.
@param rec    B-tree index record
@param index  index B-tree
@param tuple  search key
@param match  matched fields << 16 | bytes
@param comp   nonzero if ROW_FORMAT=REDUNDANT is not being used
@return the comparison result of dtuple and rec
@retval 0 if dtuple is equal to rec
@retval negative if dtuple is less than rec
@retval positive if dtuple is greater than rec */
static int cmp_dtuple_rec_bytes(const rec_t *rec,
                                const dict_index_t &index,
                                const dtuple_t &tuple, int *match, ulint comp)
  noexcept
{
  ut_ad(dtuple_check_typed(&tuple));
  ut_ad(page_rec_is_leaf(rec));
  ut_ad(!(REC_INFO_MIN_REC_FLAG & dtuple_get_info_bits(&tuple)));
  ut_ad(!!comp == index.table->not_redundant());

  if (UNIV_UNLIKELY(REC_INFO_MIN_REC_FLAG & rec_get_info_bits(rec, comp)))
  {
    ut_d(const page_t *page= page_align(rec));
    ut_ad(page_rec_is_first(rec, page));
    ut_ad(!page_has_prev(page));
    ut_ad(rec_is_metadata(rec, index));
    *match= 0;
    return 1;
  }

  ulint cur_field= *match >> 16;
  ulint cur_bytes= uint16_t(*match);
  ulint n_cmp= dtuple_get_n_fields_cmp(&tuple);
  int ret= 0;

  ut_ad(n_cmp <= dtuple_get_n_fields(&tuple));
  ut_ad(cur_field <= n_cmp);
  ut_ad(cur_field + !!cur_bytes <=
        (index.is_primary() ? index.db_roll_ptr() : index.n_core_fields));

  if (UNIV_LIKELY(comp != 0))
  {
    const byte *nulls= rec - REC_N_NEW_EXTRA_BYTES;
    const byte *lens;
    if (rec_get_status(rec) == REC_STATUS_INSTANT)
    {
      ulint n_fields= index.n_core_fields + rec_get_n_add_field(nulls) + 1;
      ut_ad(n_fields <= index.n_fields);
      const ulint n_nullable= index.get_n_nullable(n_fields);
      ut_ad(n_nullable <= index.n_nullable);
      lens= --nulls - UT_BITS_IN_BYTES(n_nullable);
    }
    else
      lens= --nulls - index.n_core_null_bytes;
    byte null_mask= 1;

    size_t i= 0;
    const dict_field_t *field= index.fields;
    const dict_field_t *const end= field + tuple.n_fields_cmp;
    const byte *f= rec;
    do
    {
      const dict_col_t *col= field->col;
      if (col->is_nullable())
      {
        const int is_null{*nulls & null_mask};
#if defined __GNUC__ && !defined __clang__
# pragma GCC diagnostic push
# if __GNUC__ < 12 || defined WITH_UBSAN
#  pragma GCC diagnostic ignored "-Wconversion"
# endif
#endif
        null_mask<<= 1;
#if defined __GNUC__ && !defined __clang__
# pragma GCC diagnostic pop
#endif
        if (UNIV_UNLIKELY(!null_mask))
          null_mask= 1, nulls--;
        if (is_null)
        {
          if (i < cur_field || tuple.fields[i].len == UNIV_SQL_NULL)
            continue;
          cur_bytes= 0;
          ret= field->descending ? -1 : 1;
          break;
        }
      }

      size_t len= field->fixed_len;

      if (!len)
      {
        len= *lens--;
        if (UNIV_UNLIKELY(len & 0x80) && DATA_BIG_COL(col))
        {
          len<<= 8;
          len|= *lens--;
          ut_ad(!(len & 0x4000));
          len&= 0x3fff;
        }
      }

      if (i >= cur_field)
      {
        const dfield_t *const df= dtuple_get_nth_field(&tuple, i);
        ut_ad(!dfield_is_ext(df));
        if (df->len == UNIV_SQL_NULL)
        {
          ut_ad(cur_bytes == 0);
        less:
          ret= field->descending ? 1 : -1;
          goto non_redundant_order_resolved;
        }

        switch (df->type.mtype) {
        case DATA_FIXBINARY:
        case DATA_BINARY:
        case DATA_INT:
        case DATA_SYS_CHILD:
        case DATA_SYS:
          break;
        case DATA_BLOB:
          if (df->type.prtype & DATA_BINARY_TYPE)
            break;
          /* fall through */
        default:
          cur_bytes= 0;
          ret= cmp_data(df->type.mtype, df->type.prtype, field->descending,
                        static_cast<const byte*>(df->data), df->len, f, len);
          if (ret)
            goto non_redundant_order_resolved;
          goto next_field;
        }

        /* Set the pointers at the current byte */
        const byte *rec_b_ptr= f + cur_bytes;
        const byte *dtuple_b_ptr=
          static_cast<const byte*>(df->data) + cur_bytes;
        /* Compare then the fields */
        for (const ulint pad= cmp_get_pad_char(df->type);; cur_bytes++)
        {
          const bool eod= df->len <= cur_bytes;
          ulint rec_byte= pad, dtuple_byte= pad;

          if (len > cur_bytes)
            rec_byte= *rec_b_ptr++;
          else if (eod)
            break;
          else if (rec_byte == ULINT_UNDEFINED)
          {
          greater:
            ret= field->descending ? -1 : 1;
            goto non_redundant_order_resolved;
          }

          if (!eod)
            dtuple_byte= *dtuple_b_ptr++;
          else if (dtuple_byte == ULINT_UNDEFINED)
            goto less;

          if (dtuple_byte == rec_byte);
          else if (dtuple_byte < rec_byte)
            goto less;
          else
            goto greater;
        }

        cur_bytes= 0;
      }

    next_field:
      f+= len;
    }
    while (i++, ++field < end);

    ut_ad(cur_bytes == 0);
  non_redundant_order_resolved:
    ut_ad(i >= cur_field);
    cur_field= i;
  }
  else
  {
    for (; cur_field < n_cmp; cur_field++)
    {
      const dfield_t *df= dtuple_get_nth_field(&tuple, cur_field);
      ut_ad(!dfield_is_ext(df));
      size_t len;
      const byte *rec_b_ptr= rec_get_nth_field_old(rec, cur_field, &len);
      /* If we have matched yet 0 bytes, it may be that one or
      both the fields are SQL null, or the record or dtuple may be
      the predefined minimum record. */
      if (df->len == UNIV_SQL_NULL)
      {
        ut_ad(cur_bytes == 0);
        if (len == UNIV_SQL_NULL)
          continue;
      redundant_less:
        ret= index.fields[cur_field].descending ? 1 : -1;
        goto order_resolved;
      }
      else if (len == UNIV_SQL_NULL)
      {
        ut_ad(cur_bytes == 0);
        /* We define the SQL null to be the smallest possible value */
      redundant_greater:
        ret= index.fields[cur_field].descending ? -1 : 1;
        goto order_resolved;
      }

      switch (df->type.mtype) {
      case DATA_FIXBINARY:
      case DATA_BINARY:
      case DATA_INT:
      case DATA_SYS_CHILD:
      case DATA_SYS:
        break;
      case DATA_BLOB:
        if (df->type.prtype & DATA_BINARY_TYPE)
          break;
        /* fall through */
      default:
        ret= cmp_data(df->type.mtype, df->type.prtype,
                      index.fields[cur_field].descending,
                      static_cast<const byte*>(df->data), df->len,
                      rec_b_ptr, len);
        cur_bytes= 0;
        if (!ret)
          continue;
        goto order_resolved;
      }

      /* Set the pointers at the current byte */
      rec_b_ptr+= cur_bytes;
      const byte *dtuple_b_ptr= static_cast<const byte*>(df->data) +
        cur_bytes;
      /* Compare then the fields */
      for (const ulint pad= cmp_get_pad_char(df->type);; cur_bytes++)
      {
        const bool eod= df->len <= cur_bytes;
        ulint rec_byte= pad, dtuple_byte= pad;

        if (len > cur_bytes)
          rec_byte= *rec_b_ptr++;
        else if (eod)
          break;
        else if (rec_byte == ULINT_UNDEFINED)
          goto redundant_greater;

        if (!eod)
          dtuple_byte= *dtuple_b_ptr++;
        else if (dtuple_byte == ULINT_UNDEFINED)
          goto redundant_less;

        if (dtuple_byte == rec_byte);
        else if (dtuple_byte < rec_byte)
          goto redundant_less;
        else if (dtuple_byte > rec_byte)
          goto redundant_greater;
      }

      cur_bytes= 0;
    }

    ut_ad(cur_bytes == 0);
  }

order_resolved:
  *match= int(cur_field << 16 | cur_bytes);
  return ret;
}

/** Try a search shortcut based on the last insert.
@param page        B-tree index leaf page
@param rec         PAGE_LAST_INSERT record
@param index       index B-tree
@param tuple       search key
@param iup_fields  matched fields in the upper limit record
@param ilow_fields matched fields in the low limit record
@param iup_bytes   matched bytes after iup_fields
@param ilow_bytes  matched bytes after ilow_fields
@return true on success */
static bool page_cur_try_search_shortcut_bytes(const page_t *page,
                                               const rec_t *rec,
                                               const dict_index_t &index,
                                               const dtuple_t &tuple,
                                               uint16_t *iup_fields,
                                               uint16_t *ilow_fields,
                                               uint16_t *iup_bytes,
                                               uint16_t *ilow_bytes) noexcept
{
  ut_ad(page_rec_is_user_rec(rec));
  int low= int(*ilow_fields << 16 | *ilow_bytes);
  int up= int(*iup_fields << 16 | *iup_bytes);
  up= low= std::min(low, up);
  const auto comp= page_is_comp(page);
  if (cmp_dtuple_rec_bytes(rec, index, tuple, &low, comp) < 0)
    return false;
  const rec_t *next;
  if (UNIV_LIKELY(comp != 0))
  {
    if (!(next= page_rec_next_get<true>(page, rec)))
      return false;
    if (next != page + PAGE_NEW_SUPREMUM)
    {
    cmp_up:
      if (cmp_dtuple_rec_bytes(rec, index, tuple, &up, comp) >= 0)
        return false;
      *iup_fields= uint16_t(up >> 16);
      *iup_bytes= uint16_t(up);
    }
  }
  else
  {
    if (!(next= page_rec_next_get<false>(page, rec)))
      return false;
    if (next != page + PAGE_OLD_SUPREMUM)
      goto cmp_up;
  }

  *ilow_fields= uint16_t(low >> 16);
  *ilow_bytes= uint16_t(low);
  return true;
}

bool page_cur_search_with_match_bytes(const dtuple_t &tuple,
                                      page_cur_mode_t mode,
                                      uint16_t *iup_fields,
                                      uint16_t *ilow_fields,
                                      page_cur_t *cursor,
                                      uint16_t *iup_bytes,
                                      uint16_t *ilow_bytes) noexcept
{
  ut_ad(dtuple_validate(&tuple));
  ut_ad(!(tuple.info_bits & REC_INFO_MIN_REC_FLAG));
  ut_ad(mode == PAGE_CUR_L || mode == PAGE_CUR_LE ||
        mode == PAGE_CUR_G || mode == PAGE_CUR_GE);
  const dict_index_t &index= *cursor->index;
  const buf_block_t *const block= cursor->block;
  const page_t *const page= block->page.frame;
  ut_ad(page_is_leaf(page));
  ut_d(page_check_dir(page));
#ifdef UNIV_ZIP_DEBUG
  if (const page_zip_des_t *page_zip= buf_block_get_page_zip(block))
    ut_a(page_zip_validate(page_zip, page, &index));
#endif /* UNIV_ZIP_DEBUG */
  const auto comp= page_is_comp(page);

  if (mode != PAGE_CUR_LE || page_get_direction(page) != PAGE_RIGHT);
  else if (uint16_t last= page_header_get_offs(page, PAGE_LAST_INSERT))
  {
    const rec_t *rec= page + last;
    if (page_header_get_field(page, PAGE_N_DIRECTION) > 2 &&
        page_cur_try_search_shortcut_bytes(page, rec, index, tuple,
                                           iup_fields, ilow_fields,
                                           iup_bytes, ilow_bytes))
    {
      page_cur_position(rec, block, cursor);
      return false;
    }
  }

  /* If mode PAGE_CUR_G is specified, we are trying to position the
  cursor to answer a query of the form "tuple < X", where tuple is the
  input parameter, and X denotes an arbitrary physical record on the
  page.  We want to position the cursor on the first X which satisfies
  the condition. */
  int up_cmp= int(*iup_fields << 16 | *iup_bytes);
  int low_cmp= int(*ilow_fields << 16 | *ilow_bytes);

  /* Perform binary search. First the search is done through the page
  directory, after that as a linear search in the list of records
  owned by the upper limit directory slot. */
  size_t low= 0, up= ulint{page_dir_get_n_slots(page)} - 1;
  const rec_t *mid_rec;

  /* Perform binary search until the lower and upper limit directory
  slots come to the distance 1 of each other */
  while (up - low > 1)
  {
    const size_t mid= (low + up) / 2;
    mid_rec= page_dir_slot_get_rec_validate(page,
                                            page_dir_get_nth_slot(page, mid));
    if (UNIV_UNLIKELY(!mid_rec))
      return true;
    int cur= std::min(low_cmp, up_cmp);
    int cmp= cmp_dtuple_rec_bytes(mid_rec, index, tuple, &cur, comp);
    if (cmp > 0)
    low_slot_match:
      low= mid, low_cmp= cur;
    else if (cmp)
    up_slot_match:
      up= mid, up_cmp= cur;
    else if (mode == PAGE_CUR_G || mode == PAGE_CUR_LE)
      goto low_slot_match;
    else
      goto up_slot_match;
  }

  const rec_t *up_rec=
    page_dir_slot_get_rec_validate(page, page_dir_get_nth_slot(page, up));
  const rec_t *low_rec=
    page_dir_slot_get_rec_validate(page, page_dir_get_nth_slot(page, low));
  if (UNIV_UNLIKELY(!low_rec || !up_rec))
    return true;

  /* Perform linear search until the upper and lower records come to
  distance 1 of each other. */

  for (;;)
  {
    mid_rec= comp
      ? page_rec_next_get<true>(page, low_rec)
      : page_rec_next_get<false>(page, low_rec);
    if (!mid_rec)
      return true;
    if (mid_rec == up_rec)
      break;

    int cur= std::min(low_cmp, up_cmp);
    int cmp;

    if (UNIV_UNLIKELY(rec_get_info_bits(mid_rec, comp) &
                      REC_INFO_MIN_REC_FLAG))
    {
      ut_ad(!page_has_prev(page));
      ut_ad(rec_is_metadata(mid_rec, index));
      goto low_rec_match;
    }

    cmp= cmp_dtuple_rec_bytes(mid_rec, index, tuple, &cur, comp);

    if (cmp > 0)
    low_rec_match:
      low_rec= mid_rec, low_cmp= cur;
    else if (cmp)
    up_rec_match:
      up_rec= mid_rec, up_cmp= cur;
    else if (mode == PAGE_CUR_G || mode == PAGE_CUR_LE)
      goto low_rec_match;
    else
      goto up_rec_match;
  }

  page_cur_position(mode <= PAGE_CUR_GE ? up_rec : low_rec, block, cursor);
  *iup_fields= uint16_t(up_cmp >> 16), *iup_bytes= uint16_t(up_cmp);
  *ilow_fields= uint16_t(low_cmp >> 16), *ilow_bytes= uint16_t(low_cmp);
  return false;
}

/** Compare a data tuple to a physical record.
@tparam leaf  whether this must be a leaf page
@param page   B-tree index page
@param rec    B-tree index record
@param index  index B-tree
@param tuple  search key
@param match  matched fields << 16 | bytes
@param comp   nonzero if ROW_FORMAT=REDUNDANT is not being used
@return the comparison result of dtuple and rec
@retval 0 if dtuple is equal to rec
@retval negative if dtuple is less than rec
@retval positive if dtuple is greater than rec */
template<bool leaf= true>
static int page_cur_dtuple_cmp(const dtuple_t &dtuple, const rec_t *rec,
                               const dict_index_t &index,
                               uint16_t *matched_fields, ulint comp) noexcept
{
  ut_ad(dtuple_check_typed(&dtuple));
  ut_ad(!!comp == index.table->not_redundant());
  ulint cur_field= *matched_fields;
  ut_ad(dtuple.n_fields_cmp > 0);
  ut_ad(dtuple.n_fields_cmp <= index.n_core_fields);
  ut_ad(cur_field <= dtuple.n_fields_cmp);
  ut_ad(leaf == page_rec_is_leaf(rec));
  ut_ad(!leaf || !(rec_get_info_bits(rec, comp) & REC_INFO_MIN_REC_FLAG) ||
        index.is_instant() ||
        (index.is_primary() && trx_roll_crash_recv_trx &&
         !trx_rollback_is_active));
  ut_ad(!leaf || !(dtuple.info_bits & REC_INFO_MIN_REC_FLAG) ||
        index.is_instant() ||
        (index.is_primary() && trx_roll_crash_recv_trx &&
         !trx_rollback_is_active));
  ut_ad(leaf || !index.is_spatial() ||
        dtuple.n_fields_cmp == DICT_INDEX_SPATIAL_NODEPTR_SIZE + 1);
  int ret= 0;

  if (dtuple.info_bits & REC_INFO_MIN_REC_FLAG)
  {
    *matched_fields= 0;
    return -!(rec_get_info_bits(rec, comp) & REC_INFO_MIN_REC_FLAG);
  }
  else if (rec_get_info_bits(rec, comp) & REC_INFO_MIN_REC_FLAG)
  {
    *matched_fields= 0;
    return 1;
  }

  if (UNIV_LIKELY(comp != 0))
  {
    const byte *nulls= rec - REC_N_NEW_EXTRA_BYTES;
    const byte *lens;
    if (rec_get_status(rec) == REC_STATUS_INSTANT)
    {
      ulint n_fields= index.n_core_fields + rec_get_n_add_field(nulls) + 1;
      ut_ad(n_fields <= index.n_fields);
      const ulint n_nullable= index.get_n_nullable(n_fields);
      ut_ad(n_nullable <= index.n_nullable);
      lens= --nulls - UT_BITS_IN_BYTES(n_nullable);
    }
    else
      lens= --nulls - index.n_core_null_bytes;
    byte null_mask= 1;

    size_t i= 0;
    const dict_field_t *field= index.fields;
    const dict_field_t *const end= field + dtuple.n_fields_cmp;
    const byte *f= rec;
    do
    {
      const dict_col_t *col= field->col;
      if (col->is_nullable())
      {
        const int is_null{*nulls & null_mask};
#if defined __GNUC__ && !defined __clang__
# pragma GCC diagnostic push
# if __GNUC__ < 12 || defined WITH_UBSAN
#  pragma GCC diagnostic ignored "-Wconversion"
# endif
#endif
        null_mask<<= 1;
#if defined __GNUC__ && !defined __clang__
# pragma GCC diagnostic pop
#endif
        if (UNIV_UNLIKELY(!null_mask))
          null_mask= 1, nulls--;
        if (is_null)
        {
          if (i < cur_field || dtuple.fields[i].len == UNIV_SQL_NULL)
            continue;
          ret= field->descending ? -1 : 1;
          break;
        }
      }

      size_t len= field->fixed_len;

      if (!len)
      {
        len= *lens--;
        if (UNIV_UNLIKELY(len & 0x80) && DATA_BIG_COL(col))
        {
          len<<= 8;
          len|= *lens--;
          ut_ad(!(len & 0x4000));
          len&= 0x3fff;
        }
      }

      if (i >= cur_field)
      {
        const dfield_t *df= dtuple_get_nth_field(&dtuple, i);
        ut_ad(!dfield_is_ext(df));
        if (!leaf && i == DICT_INDEX_SPATIAL_NODEPTR_SIZE &&
            index.is_spatial())
        {
          /* SPATIAL INDEX non-leaf records comprise
          MBR (minimum bounding rectangle) and the child page number.
          The function rtr_cur_restore_position() includes the
          child page number in the search key, because the MBR alone
          would not be unique. */
          ut_ad(dtuple.fields[DICT_INDEX_SPATIAL_NODEPTR_SIZE].len == 4);
          len= 4;
        }
        ret= cmp_data(df->type.mtype, df->type.prtype, field->descending,
                      static_cast<const byte*>(df->data), df->len, f, len);
        if (ret)
          break;
      }

      f+= len;
    }
    while (i++, ++field < end);
    ut_ad(i >= cur_field);
    *matched_fields= uint16_t(i);
  }
  else
  {
    for (; cur_field < dtuple.n_fields_cmp; cur_field++)
    {
      const dfield_t *df= dtuple_get_nth_field(&dtuple, cur_field);
      ut_ad(!dfield_is_ext(df));
      size_t len;
      const byte *f= rec_get_nth_field_old(rec, cur_field, &len);
      ret= cmp_data(df->type.mtype, df->type.prtype,
                    index.fields[cur_field].descending,
                    static_cast<const byte*>(df->data), df->len, f, len);
      if (ret)
        break;
    }
    *matched_fields= uint16_t(cur_field);
  }
  return ret;
}

static int page_cur_dtuple_cmp(const dtuple_t &dtuple, const rec_t *rec,
                               const dict_index_t &index,
                               uint16_t *matched_fields, ulint comp, bool leaf)
  noexcept
{
  return leaf
    ? page_cur_dtuple_cmp<true>(dtuple, rec, index, matched_fields, comp)
    : page_cur_dtuple_cmp<false>(dtuple, rec, index, matched_fields, comp);
}

#ifdef BTR_CUR_HASH_ADAPT
bool btr_cur_t::check_mismatch(const dtuple_t &tuple, bool ge, ulint comp)
  noexcept
{
  ut_ad(page_is_leaf(page_cur.block->page.frame));
  ut_ad(page_rec_is_user_rec(page_cur.rec));

  const rec_t *rec= page_cur.rec;
  uint16_t match= 0;
  int cmp= page_cur_dtuple_cmp(tuple, rec, *index(), &match, comp);
  const auto uniq= dict_index_get_n_unique_in_tree(index());
  ut_ad(match <= uniq);
  ut_ad(match <= tuple.n_fields_cmp);
  ut_ad(match < uniq || !cmp);

  const page_t *const page= page_cur.block->page.frame;

  if (UNIV_LIKELY(!ge))
  {
    if (cmp < 0)
      return true;
    low_match= match;
    up_match= 0;
    if (UNIV_LIKELY(comp != 0))
    {
      rec= page_rec_next_get<true>(page, rec);
      if (!rec)
        return true;
      if (uintptr_t(rec - page) == PAGE_NEW_SUPREMUM)
      le_supremum:
        /* If we matched the full key at the end of a page (but not the index),
        the adaptive hash index was successful. */
        return page_has_next(page) && match < uniq;
      switch (rec_get_status(rec)) {
      case REC_STATUS_INSTANT:
      case REC_STATUS_ORDINARY:
        break;
      default:
        return true;
      }
    }
    else
    {
      rec= page_rec_next_get<false>(page, rec);
      if (!rec)
        return true;
      if (uintptr_t(rec - page) == PAGE_OLD_SUPREMUM)
        goto le_supremum;
    }
    return page_cur_dtuple_cmp(tuple, rec, *index(), &up_match, comp) >= 0;
  }
  else
  {
    if (cmp > 0)
      return true;
    up_match= match;
    if (match >= uniq)
      return false;
    match= 0;
    if (!(rec= page_rec_get_prev_const(rec)))
      return true;
    if (uintptr_t(rec - page) == (comp ? PAGE_NEW_INFIMUM : PAGE_OLD_INFIMUM))
      return page_has_prev(page);
    if (UNIV_LIKELY(comp != 0))
      switch (rec_get_status(rec)) {
      case REC_STATUS_INSTANT:
      case REC_STATUS_ORDINARY:
        break;
      default:
        return true;
      }
    return page_cur_dtuple_cmp(tuple, rec, *index(), &match, comp) <= 0;
  }
}
#endif /* BTR_CUR_HASH_ADAPT */

/** Try a search shortcut based on the last insert.
@param page   index page
@param rec    PAGE_LAST_INSERT record
@param index  index tree
@param tuple  search key
@param iup    matched fields in the upper limit record
@param ilow   matched fields in the lower limit record
@param comp   nonzero if ROW_FORMAT=REDUNDANT is not being used
@return record
@return nullptr if the tuple was not found */
static bool page_cur_try_search_shortcut(const page_t *page, const rec_t *rec,
                                         const dict_index_t &index,
                                         const dtuple_t &tuple,
                                         uint16_t *iup, uint16_t *ilow,
                                         ulint comp) noexcept
{
  ut_ad(dtuple_check_typed(&tuple));
  ut_ad(page_rec_is_user_rec(rec));

  uint16_t low= std::min(*ilow, *iup), up= low;

  if (page_cur_dtuple_cmp(tuple, rec, index, &low, comp) < 0)
    return false;

  if (comp)
  {
    rec= page_rec_next_get<true>(page, rec);
    if (!rec)
      return false;
    if (rec != page + PAGE_NEW_SUPREMUM)
    {
    compare_next:
      if (page_cur_dtuple_cmp(tuple, rec, index, &up, comp) >= 0)
        return false;
      *iup= up;
    }
  }
  else
  {
    rec= page_rec_next_get<false>(page, rec);
    if (!rec)
      return false;
    if (rec != page + PAGE_OLD_SUPREMUM)
      goto compare_next;
  }

  *ilow= low;
  return true;
}

bool page_cur_search_with_match(const dtuple_t *tuple, page_cur_mode_t mode,
                                uint16_t *iup_fields, uint16_t *ilow_fields,
                                page_cur_t *cursor, rtr_info_t *rtr_info)
  noexcept
{
  ut_ad(dtuple_validate(tuple));
  ut_ad(mode == PAGE_CUR_L || mode == PAGE_CUR_LE ||
        mode == PAGE_CUR_G || mode == PAGE_CUR_GE ||
        cursor->index->is_spatial());
  const dict_index_t &index= *cursor->index;
  const buf_block_t *const block= cursor->block;
  const page_t *const page= block->page.frame;
  ut_d(page_check_dir(page));
#ifdef UNIV_ZIP_DEBUG
  if (const page_zip_des_t *page_zip= buf_block_get_page_zip(block))
    ut_a(page_zip_validate(page_zip, page, &index));
#endif /* UNIV_ZIP_DEBUG */
  const auto comp= page_is_comp(page);
  const bool leaf{page_is_leaf(page)};

  /* If the mode is for R-tree indexes, use the special MBR
  related compare functions */
  if (mode == PAGE_CUR_RTREE_INSERT && leaf)
  {
    /* Leaf level insert uses the traditional compare function */
    mode= PAGE_CUR_LE;
    goto check_last_insert;
  }
  else if (mode > PAGE_CUR_LE)
    return rtr_cur_search_with_match(block,
                                     const_cast<dict_index_t*>(&index),
                                     tuple, mode, cursor, rtr_info);
  else if (mode == PAGE_CUR_LE && leaf)
  {
  check_last_insert:
    if (page_get_direction(page) != PAGE_RIGHT ||
        (tuple->info_bits & REC_INFO_MIN_REC_FLAG));
    else if (uint16_t last= page_header_get_offs(page, PAGE_LAST_INSERT))
    {
      const rec_t *rec= page + last;
      if (page_header_get_field(page, PAGE_N_DIRECTION) > 2 &&
          page_cur_try_search_shortcut(page, rec, index, *tuple,
                                       iup_fields, ilow_fields, comp))
      {
        page_cur_position(rec, block, cursor);
        return false;
      }
    }
  }

  /* If mode PAGE_CUR_G is specified, we are trying to position the
  cursor to answer a query of the form "tuple < X", where tuple is the
  input parameter, and X denotes an arbitrary physical record on the
  page.  We want to position the cursor on the first X which satisfies
  the condition. */
  uint16_t up_fields= *iup_fields, low_fields= *ilow_fields;

  /* Perform binary search. First the search is done through the page
  directory, after that as a linear search in the list of records
  owned by the upper limit directory slot. */
  size_t low= 0, up= ulint{page_dir_get_n_slots(page)} - 1;
  const rec_t *mid_rec;

  /* Perform binary search until the lower and upper limit directory
  slots come to the distance 1 of each other */
  while (up - low > 1)
  {
    const size_t mid= (low + up) / 2;
    mid_rec=
      page_dir_slot_get_rec_validate(page, page_dir_get_nth_slot(page, mid));
    if (UNIV_UNLIKELY(!mid_rec))
      return true;
    uint16_t cur= std::min(low_fields, up_fields);
    int cmp= page_cur_dtuple_cmp(*tuple, mid_rec, index, &cur, comp, leaf);
    if (cmp > 0)
    low_slot_match:
      low= mid, low_fields= cur;
    else if (cmp)
    up_slot_match:
      up= mid, up_fields= cur;
    else if (mode == PAGE_CUR_G || mode == PAGE_CUR_LE)
      goto low_slot_match;
    else
      goto up_slot_match;
  }

  const rec_t *up_rec=
    page_dir_slot_get_rec_validate(page, page_dir_get_nth_slot(page, up));;
  const rec_t *low_rec=
    page_dir_slot_get_rec_validate(page, page_dir_get_nth_slot(page, low));
  if (UNIV_UNLIKELY(!low_rec || !up_rec))
    return true;

  /* Perform linear search until the upper and lower records come to
  distance 1 of each other. */

  for (;;)
  {
    mid_rec= comp
      ? page_rec_next_get<true>(page, low_rec)
      : page_rec_next_get<false>(page, low_rec);
    if (!mid_rec)
      return true;
    if (mid_rec == up_rec)
      break;

    uint16_t cur= std::min(low_fields, up_fields);
    int cmp= page_cur_dtuple_cmp(*tuple, mid_rec, index, &cur, comp, leaf);
    if (cmp > 0)
    low_rec_match:
      low_rec= mid_rec, low_fields= cur;
    else if (cmp)
    up_rec_match:
      up_rec= mid_rec, up_fields= cur;
    else if (mode == PAGE_CUR_G || mode == PAGE_CUR_LE)
    {
      if (cur == 0)
      {
        /* A match on 0 fields must be due to REC_INFO_MIN_REC_FLAG */
        ut_ad(rec_get_info_bits(mid_rec, comp) & REC_INFO_MIN_REC_FLAG);
        ut_ad(!page_has_prev(page));
        ut_ad(!leaf || rec_is_metadata(mid_rec, index));
        cur= tuple->n_fields_cmp;
      }
      goto low_rec_match;
    }
    else
      goto up_rec_match;
  }

  page_cur_position(mode <= PAGE_CUR_GE ? up_rec : low_rec, block, cursor);
  *iup_fields= up_fields;
  *ilow_fields= low_fields;
  return false;
}

/***********************************************************//**
Positions a page cursor on a randomly chosen user record on a page. If there
are no user records, sets the cursor on the infimum record. */
void page_cur_open_on_rnd_user_rec(page_cur_t *cursor)
{
  if (const ulint n_recs= page_get_n_recs(cursor->block->page.frame))
    if ((cursor->rec= page_rec_get_nth(cursor->block->page.frame,
                                       ut_rnd_interval(n_recs) + 1)))
      return;
  cursor->rec= page_get_infimum_rec(cursor->block->page.frame);
}

/**
Set the number of owned records.
@param[in,out]  rec     record in block.frame
@param[in]      n_owned number of records skipped in the sparse page directory
@param[in]      comp    whether ROW_FORMAT is COMPACT or DYNAMIC */
static void page_rec_set_n_owned(rec_t *rec, ulint n_owned, bool comp)
{
  rec-= comp ? REC_NEW_N_OWNED : REC_OLD_N_OWNED;
  *rec= static_cast<byte>((*rec & ~REC_N_OWNED_MASK) |
                          (n_owned << REC_N_OWNED_SHIFT));
}

/**
Split a directory slot which owns too many records.
@param[in,out]  block   index page
@param[in,out]  slot    the slot that needs to be split */
static bool page_dir_split_slot(const buf_block_t &block,
                                page_dir_slot_t *slot)
{
  ut_ad(slot <= &block.page.frame[srv_page_size - PAGE_EMPTY_DIR_START]);
  slot= my_assume_aligned<2>(slot);

  const ulint n_owned= PAGE_DIR_SLOT_MAX_N_OWNED + 1;

  ut_ad(page_dir_slot_get_n_owned(slot) == n_owned);
  static_assert((PAGE_DIR_SLOT_MAX_N_OWNED + 1) / 2 >=
                PAGE_DIR_SLOT_MIN_N_OWNED, "compatibility");

  /* Find a record approximately in the middle. */
  const rec_t *rec= page_dir_slot_get_rec_validate(block.page.frame,
                                                   slot + PAGE_DIR_SLOT_SIZE);

  for (ulint i= n_owned / 2; i--; )
  {
    if (UNIV_UNLIKELY(!rec))
      return true;
    rec= page_rec_get_next_const(rec);
  }

  if (UNIV_UNLIKELY(!rec))
    return true;

  /* Add a directory slot immediately below this one. */
  constexpr uint16_t n_slots_f= PAGE_N_DIR_SLOTS + PAGE_HEADER;
  byte *n_slots_p= my_assume_aligned<2>(n_slots_f + block.page.frame);
  const uint16_t n_slots= mach_read_from_2(n_slots_p);

  page_dir_slot_t *last_slot= static_cast<page_dir_slot_t*>
    (block.page.frame + srv_page_size - (PAGE_DIR + PAGE_DIR_SLOT_SIZE) -
     n_slots * PAGE_DIR_SLOT_SIZE);

  if (UNIV_UNLIKELY(slot < last_slot))
    return true;

  memmove_aligned<2>(last_slot, last_slot + PAGE_DIR_SLOT_SIZE,
                     slot - last_slot);

  const ulint half_owned= n_owned / 2;

  mach_write_to_2(n_slots_p, n_slots + 1);

  mach_write_to_2(slot, rec - block.page.frame);
  const bool comp= page_is_comp(block.page.frame) != 0;
  page_rec_set_n_owned(page_dir_slot_get_rec(block.page.frame, slot),
                       half_owned, comp);
  page_rec_set_n_owned(page_dir_slot_get_rec(block.page.frame,
                                             slot - PAGE_DIR_SLOT_SIZE),
                       n_owned - half_owned, comp);
  return false;
}

/**
Split a directory slot which owns too many records.
@param[in,out]  block   index page (ROW_FORMAT=COMPRESSED)
@param[in]      s       the slot that needs to be split
@param[in,out]  mtr     mini-transaction */
static void page_zip_dir_split_slot(buf_block_t *block, ulint s, mtr_t* mtr)
{
  ut_ad(block->page.zip.data);
  ut_ad(page_is_comp(block->page.frame));
  ut_ad(s);

  page_dir_slot_t *slot= page_dir_get_nth_slot(block->page.frame, s);
  const ulint n_owned= PAGE_DIR_SLOT_MAX_N_OWNED + 1;

  ut_ad(page_dir_slot_get_n_owned(slot) == n_owned);
  static_assert((PAGE_DIR_SLOT_MAX_N_OWNED + 1) / 2 >=
                PAGE_DIR_SLOT_MIN_N_OWNED, "compatibility");

  /* 1. We loop to find a record approximately in the middle of the
  records owned by the slot. */

  const rec_t *rec= page_dir_slot_get_rec(block->page.frame,
                                          slot + PAGE_DIR_SLOT_SIZE);

  /* We do not try to prevent crash on corruption here.
  For ROW_FORMAT=COMPRESSED pages, the next-record links should
  be validated in page_zip_decompress(). Corruption should only
  be possible here if the buffer pool was corrupted later. */
  for (ulint i= n_owned / 2; i--; )
    rec= page_rec_get_next_const(rec);

  /* Add a directory slot immediately below this one. */
  constexpr uint16_t n_slots_f= PAGE_N_DIR_SLOTS + PAGE_HEADER;
  byte *n_slots_p= my_assume_aligned<2>(n_slots_f + block->page.frame);
  const uint16_t n_slots= mach_read_from_2(n_slots_p);

  page_dir_slot_t *last_slot= static_cast<page_dir_slot_t*>
    (block->page.frame + srv_page_size - (PAGE_DIR + PAGE_DIR_SLOT_SIZE) -
     n_slots * PAGE_DIR_SLOT_SIZE);
  memmove_aligned<2>(last_slot, last_slot + PAGE_DIR_SLOT_SIZE,
                     slot - last_slot);

  const ulint half_owned= n_owned / 2;

  mtr->write<2>(*block, n_slots_p, 1U + n_slots);

  /* Log changes to the compressed page header and the dense page directory. */
  memcpy_aligned<2>(&block->page.zip.data[n_slots_f], n_slots_p, 2);
  mach_write_to_2(slot, rec - block->page.frame);
  page_rec_set_n_owned<true>(block,
                             page_dir_slot_get_rec(block->page.frame, slot),
                             half_owned,
                             true, mtr);
  page_rec_set_n_owned<true>(block,
                             page_dir_slot_get_rec(block->page.frame,
                                                   slot - PAGE_DIR_SLOT_SIZE),
                             n_owned - half_owned, true, mtr);
}

/**
Try to balance an underfilled directory slot with an adjacent one,
so that there are at least the minimum number of records owned by the slot;
this may result in merging the two slots.
@param[in,out]	block		ROW_FORMAT=COMPRESSED page
@param[in]	s		the slot to be balanced
@param[in,out]	mtr		mini-transaction */
static void page_zip_dir_balance_slot(buf_block_t *block, ulint s, mtr_t *mtr)
{
	ut_ad(block->page.zip.data);
	ut_ad(page_is_comp(block->page.frame));
	ut_ad(s > 0);

	const ulint n_slots = page_dir_get_n_slots(block->page.frame);

	if (UNIV_UNLIKELY(s + 1 == n_slots)) {
		/* The last directory slot cannot be balanced. */
		return;
	}

	ut_ad(s < n_slots);

	page_dir_slot_t* slot = page_dir_get_nth_slot(block->page.frame, s);
	rec_t* const up_rec = const_cast<rec_t*>
		(page_dir_slot_get_rec(block->page.frame,
                                       slot - PAGE_DIR_SLOT_SIZE));
	rec_t* const slot_rec = const_cast<rec_t*>
		(page_dir_slot_get_rec(block->page.frame,
                                       slot));
	const ulint up_n_owned = rec_get_n_owned_new(up_rec);

	ut_ad(rec_get_n_owned_new(page_dir_slot_get_rec(block->page.frame,
                                                        slot))
	      == PAGE_DIR_SLOT_MIN_N_OWNED - 1);

	if (up_n_owned <= PAGE_DIR_SLOT_MIN_N_OWNED) {
		compile_time_assert(2 * PAGE_DIR_SLOT_MIN_N_OWNED - 1
				    <= PAGE_DIR_SLOT_MAX_N_OWNED);
		/* Merge the slots. */
		page_rec_set_n_owned<true>(block, slot_rec, 0, true, mtr);
		page_rec_set_n_owned<true>(block, up_rec, up_n_owned
					   + (PAGE_DIR_SLOT_MIN_N_OWNED - 1),
					   true, mtr);
		/* Shift the slots */
		page_dir_slot_t* last_slot = page_dir_get_nth_slot(
			block->page.frame, n_slots - 1);
		memmove_aligned<2>(last_slot + PAGE_DIR_SLOT_SIZE, last_slot,
				   slot - last_slot);
		constexpr uint16_t n_slots_f = PAGE_N_DIR_SLOTS + PAGE_HEADER;
		byte *n_slots_p= my_assume_aligned<2>
			(n_slots_f + block->page.frame);
		mtr->write<2>(*block, n_slots_p, n_slots - 1);
		memcpy_aligned<2>(n_slots_f + block->page.zip.data,
				  n_slots_p, 2);
		memset_aligned<2>(last_slot, 0, 2);
		return;
	}

	/* Transfer one record to the underfilled slot */
	page_rec_set_n_owned<true>(block, slot_rec, 0, true, mtr);
	const rec_t* new_rec = page_rec_next_get<true>(block->page.frame,
						       slot_rec);
	/* We do not try to prevent crash on corruption here.
	For ROW_FORMAT=COMPRESSED pages, the next-record links should
	be validated in page_zip_decompress(). Corruption should only
	be possible here if the buffer pool was corrupted later. */
	page_rec_set_n_owned<true>(block, const_cast<rec_t*>(new_rec),
				   PAGE_DIR_SLOT_MIN_N_OWNED,
				   true, mtr);
	mach_write_to_2(slot, new_rec - block->page.frame);
	page_rec_set_n_owned(up_rec, up_n_owned - 1, true);
}

/**
Try to balance an underfilled directory slot with an adjacent one,
so that there are at least the minimum number of records owned by the slot;
this may result in merging the two slots.
@param[in,out]	block		index page
@param[in]	s		the slot to be balanced */
static void page_dir_balance_slot(const buf_block_t &block, ulint s)
{
	const bool comp= page_is_comp(block.page.frame);
	ut_ad(!block.page.zip.data);
	ut_ad(s > 0);

	const ulint n_slots = page_dir_get_n_slots(block.page.frame);

	if (UNIV_UNLIKELY(s + 1 == n_slots)) {
		/* The last directory slot cannot be balanced. */
		return;
	}

	ut_ad(s < n_slots);

	page_dir_slot_t* slot = page_dir_get_nth_slot(block.page.frame, s);
	rec_t* const up_rec = const_cast<rec_t*>
		(page_dir_slot_get_rec(block.page.frame,
				       slot - PAGE_DIR_SLOT_SIZE));
	rec_t* const slot_rec = const_cast<rec_t*>
		(page_dir_slot_get_rec(block.page.frame, slot));
	const ulint up_n_owned = comp
		? rec_get_n_owned_new(up_rec)
		: rec_get_n_owned_old(up_rec);

	ut_ad(page_dir_slot_get_n_owned(slot)
	      == PAGE_DIR_SLOT_MIN_N_OWNED - 1);

	if (up_n_owned <= PAGE_DIR_SLOT_MIN_N_OWNED) {
		compile_time_assert(2 * PAGE_DIR_SLOT_MIN_N_OWNED - 1
				    <= PAGE_DIR_SLOT_MAX_N_OWNED);
		/* Merge the slots. */
		page_rec_set_n_owned(slot_rec, 0, comp);
		page_rec_set_n_owned(up_rec, up_n_owned
				     + (PAGE_DIR_SLOT_MIN_N_OWNED - 1), comp);
		/* Shift the slots */
		page_dir_slot_t* last_slot = page_dir_get_nth_slot(
			block.page.frame, n_slots - 1);
		memmove_aligned<2>(last_slot + PAGE_DIR_SLOT_SIZE, last_slot,
				   slot - last_slot);
		memset_aligned<2>(last_slot, 0, 2);
		constexpr uint16_t n_slots_f = PAGE_N_DIR_SLOTS + PAGE_HEADER;
		byte *n_slots_p= my_assume_aligned<2>
			(n_slots_f + block.page.frame);
		mach_write_to_2(n_slots_p, n_slots - 1);
		return;
	}

	/* Transfer one record to the underfilled slot */
	const rec_t* new_rec;

	if (comp) {
		if (UNIV_UNLIKELY(!(new_rec =
				    page_rec_next_get<true>(block.page.frame,
							    slot_rec)))) {
			ut_ad("corrupted page" == 0);
			return;
		}
		page_rec_set_n_owned(slot_rec, 0, true);
		page_rec_set_n_owned(const_cast<rec_t*>(new_rec),
				     PAGE_DIR_SLOT_MIN_N_OWNED, true);
		page_rec_set_n_owned(up_rec, up_n_owned - 1, true);
	} else {
		if (UNIV_UNLIKELY(!(new_rec =
				    page_rec_next_get<false>(block.page.frame,
							     slot_rec)))) {
			ut_ad("corrupted page" == 0);
			return;
		}
		page_rec_set_n_owned(slot_rec, 0, false);
		page_rec_set_n_owned(const_cast<rec_t*>(new_rec),
				     PAGE_DIR_SLOT_MIN_N_OWNED, false);
		page_rec_set_n_owned(up_rec, up_n_owned - 1, false);
	}

	mach_write_to_2(slot, new_rec - block.page.frame);
}

/** Allocate space for inserting an index record.
@tparam compressed  whether to update the ROW_FORMAT=COMPRESSED
@param[in,out]	block		index page
@param[in]	need		number of bytes needed
@param[out]	heap_no		record heap number
@return	pointer to the start of the allocated buffer
@retval	NULL	if allocation fails */
template<bool compressed=false>
static byte* page_mem_alloc_heap(buf_block_t *block, ulint need,
                                 ulint *heap_no)
{
  ut_ad(!compressed || block->page.zip.data);

  byte *heap_top= my_assume_aligned<2>(PAGE_HEAP_TOP + PAGE_HEADER +
                                       block->page.frame);

  const uint16_t top= mach_read_from_2(heap_top);

  if (need > page_get_max_insert_size(block->page.frame, 1))
    return NULL;

  byte *n_heap= my_assume_aligned<2>
    (PAGE_N_HEAP + PAGE_HEADER + block->page.frame);

  const uint16_t h= mach_read_from_2(n_heap);
  if (UNIV_UNLIKELY((h + 1) & 0x6000))
  {
    /* At the minimum record size of 5+2 bytes, we can only reach this
    condition when using innodb_page_size=64k. */
    ut_ad((h & 0x7fff) == 8191);
    ut_ad(srv_page_size == 65536);
    return NULL;
  }

  *heap_no= h & 0x7fff;
  ut_ad(*heap_no < srv_page_size / REC_N_NEW_EXTRA_BYTES);
  compile_time_assert(UNIV_PAGE_SIZE_MAX / REC_N_NEW_EXTRA_BYTES < 0x3fff);

  mach_write_to_2(heap_top, top + need);
  mach_write_to_2(n_heap, h + 1);

  if (compressed)
  {
    ut_ad(h & 0x8000);
    memcpy_aligned<4>(&block->page.zip.data[PAGE_HEAP_TOP + PAGE_HEADER],
                      heap_top, 4);
  }

  return &block->page.frame[top];
}

/** Write log for inserting a B-tree or R-tree record in
ROW_FORMAT=REDUNDANT.
@param block      B-tree or R-tree page
@param reuse      false=allocate from PAGE_HEAP_TOP; true=reuse PAGE_FREE
@param prev_rec   byte offset of the predecessor of the record to insert,
                  starting from PAGE_OLD_INFIMUM
@param info_bits  info_bits of the record
@param n_fields_s number of fields << 1 | rec_get_1byte_offs_flag()
@param hdr_c      number of common record header bytes with prev_rec
@param data_c     number of common data bytes with prev_rec
@param hdr        record header bytes to copy to the log
@param hdr_l      number of copied record header bytes
@param data       record payload bytes to copy to the log
@param data_l     number of copied record data bytes */
inline void mtr_t::page_insert(const buf_block_t &block, bool reuse,
                               ulint prev_rec, byte info_bits,
                               ulint n_fields_s, size_t hdr_c, size_t data_c,
                               const byte *hdr, size_t hdr_l,
                               const byte *data, size_t data_l)
{
  ut_ad(!block.page.zip.data);
  ut_ad(m_log_mode == MTR_LOG_ALL);
  ut_d(ulint n_slots= page_dir_get_n_slots(block.page.frame));
  ut_ad(n_slots >= 2);
  ut_d(const byte *page_end=
       page_dir_get_nth_slot(block.page.frame, n_slots - 1));
  ut_ad(&block.page.frame[prev_rec + PAGE_OLD_INFIMUM] <= page_end);
  ut_ad(block.page.frame +
        page_header_get_offs(block.page.frame, PAGE_HEAP_TOP) <= page_end);
  ut_ad(fil_page_index_page_check(block.page.frame));
  ut_ad(!(~(REC_INFO_MIN_REC_FLAG | REC_INFO_DELETED_FLAG) & info_bits));
  ut_ad(n_fields_s >= 2);
  ut_ad((n_fields_s >> 1) <= REC_MAX_N_FIELDS);
  ut_ad(data_l + data_c <= REDUNDANT_REC_MAX_DATA_SIZE);

  set_modified(block);

  static_assert(REC_INFO_MIN_REC_FLAG == 0x10, "compatibility");
  static_assert(REC_INFO_DELETED_FLAG == 0x20, "compatibility");
  n_fields_s= (n_fields_s - 2) << 2 | info_bits >> 4;

  size_t len= prev_rec < MIN_2BYTE ? 2 : prev_rec < MIN_3BYTE ? 3 : 4;
  static_assert((REC_MAX_N_FIELDS << 1 | 1) <= MIN_3BYTE, "compatibility");
  len+= n_fields_s < MIN_2BYTE ? 1 : 2;
  len+= hdr_c < MIN_2BYTE ? 1 : 2;
  static_assert(REDUNDANT_REC_MAX_DATA_SIZE <= MIN_3BYTE, "compatibility");
  len+= data_c < MIN_2BYTE ? 1 : 2;
  len+= hdr_l + data_l;

  const bool small= len < mtr_buf_t::MAX_DATA_SIZE - (1 + 3 + 3 + 5 + 5);
  byte *l= log_write<EXTENDED>(block.page.id(), &block.page, len, small);

  if (UNIV_LIKELY(small))
  {
    ut_d(const byte * const end = l + len);
    *l++= reuse ? INSERT_REUSE_REDUNDANT : INSERT_HEAP_REDUNDANT;
    l= mlog_encode_varint(l, prev_rec);
    l= mlog_encode_varint(l, n_fields_s);
    l= mlog_encode_varint(l, hdr_c);
    l= mlog_encode_varint(l, data_c);
    ::memcpy(l, hdr, hdr_l);
    l+= hdr_l;
    ::memcpy(l, data, data_l);
    l+= data_l;
    ut_ad(end == l);
    m_log.close(l);
  }
  else
  {
    m_log.close(l);
    l= m_log.open(len - hdr_l - data_l);
    ut_d(const byte * const end = l + len - hdr_l - data_l);
    *l++= reuse ? INSERT_REUSE_REDUNDANT : INSERT_HEAP_REDUNDANT;
    l= mlog_encode_varint(l, prev_rec);
    l= mlog_encode_varint(l, n_fields_s);
    l= mlog_encode_varint(l, hdr_c);
    l= mlog_encode_varint(l, data_c);
    ut_ad(end == l);
    m_log.close(l);
    m_log.push(hdr, static_cast<uint32_t>(hdr_l));
    m_log.push(data, static_cast<uint32_t>(data_l));
  }

  m_last_offset= FIL_PAGE_TYPE;
}

/** Write log for inserting a B-tree or R-tree record in
ROW_FORMAT=COMPACT or ROW_FORMAT=DYNAMIC.
@param block       B-tree or R-tree page
@param reuse       false=allocate from PAGE_HEAP_TOP; true=reuse PAGE_FREE
@param prev_rec    byte offset of the predecessor of the record to insert,
                   starting from PAGE_NEW_INFIMUM
@param info_status rec_get_info_and_status_bits()
@param shift       unless !reuse: number of bytes the PAGE_FREE is moving
@param hdr_c       number of common record header bytes with prev_rec
@param data_c      number of common data bytes with prev_rec
@param hdr         record header bytes to copy to the log
@param hdr_l       number of copied record header bytes
@param data        record payload bytes to copy to the log
@param data_l      number of copied record data bytes */
inline void mtr_t::page_insert(const buf_block_t &block, bool reuse,
                               ulint prev_rec, byte info_status,
                               ssize_t shift, size_t hdr_c, size_t data_c,
                               const byte *hdr, size_t hdr_l,
                               const byte *data, size_t data_l)
{
  ut_ad(!block.page.zip.data);
  ut_ad(m_log_mode == MTR_LOG_ALL);
  ut_d(ulint n_slots= page_dir_get_n_slots(block.page.frame));
  ut_ad(n_slots >= 2);
  ut_d(const byte *page_end= page_dir_get_nth_slot(block.page.frame,
                                                   n_slots - 1));
  ut_ad(&block.page.frame[prev_rec + PAGE_NEW_INFIMUM] <= page_end);
  ut_ad(block.page.frame +
        page_header_get_offs(block.page.frame, PAGE_HEAP_TOP) <= page_end);
  ut_ad(fil_page_index_page_check(block.page.frame));
  ut_ad(hdr_l + hdr_c + data_l + data_c <= static_cast<size_t>
        (page_end - &block.page.frame[PAGE_NEW_SUPREMUM_END]));
  ut_ad(reuse || shift == 0);
#ifdef UNIV_DEBUG
  switch (~(REC_INFO_MIN_REC_FLAG | REC_INFO_DELETED_FLAG) & info_status) {
  default:
    ut_ad(0);
    break;
  case REC_STATUS_NODE_PTR:
    ut_ad(!page_is_leaf(block.page.frame));
    break;
  case REC_STATUS_INSTANT:
  case REC_STATUS_ORDINARY:
    ut_ad(page_is_leaf(block.page.frame));
  }
#endif

  set_modified(block);

  static_assert(REC_INFO_MIN_REC_FLAG == 0x10, "compatibility");
  static_assert(REC_INFO_DELETED_FLAG == 0x20, "compatibility");
  static_assert(REC_STATUS_INSTANT == 4, "compatibility");

  const size_t enc_hdr_l= hdr_l << 3 |
    (info_status & REC_STATUS_INSTANT) | info_status >> 4;
  size_t len= prev_rec < MIN_2BYTE ? 2 : prev_rec < MIN_3BYTE ? 3 : 4;
  static_assert(REC_MAX_N_FIELDS * 2 < MIN_3BYTE, "compatibility");
  if (reuse)
  {
    if (shift < 0)
      shift= -shift << 1 | 1;
    else
      shift<<= 1;
    len+= static_cast<size_t>(shift) < MIN_2BYTE
      ? 1 : static_cast<size_t>(shift) < MIN_3BYTE ? 2 : 3;
  }
  ut_ad(hdr_c + hdr_l <= REC_MAX_N_FIELDS * 2);
  len+= hdr_c < MIN_2BYTE ? 1 : 2;
  len+= enc_hdr_l < MIN_2BYTE ? 1 : enc_hdr_l < MIN_3BYTE ? 2 : 3;
  len+= data_c < MIN_2BYTE ? 1 : data_c < MIN_3BYTE ? 2 : 3;
  len+= hdr_l + data_l;

  const bool small= len < mtr_buf_t::MAX_DATA_SIZE - (1 + 3 + 3 + 5 + 5);
  byte *l= log_write<EXTENDED>(block.page.id(), &block.page, len, small);

  if (UNIV_LIKELY(small))
  {
    ut_d(const byte * const end = l + len);
    *l++= reuse ? INSERT_REUSE_DYNAMIC : INSERT_HEAP_DYNAMIC;
    l= mlog_encode_varint(l, prev_rec);
    if (reuse)
      l= mlog_encode_varint(l, shift);
    l= mlog_encode_varint(l, enc_hdr_l);
    l= mlog_encode_varint(l, hdr_c);
    l= mlog_encode_varint(l, data_c);
    ::memcpy(l, hdr, hdr_l);
    l+= hdr_l;
    ::memcpy(l, data, data_l);
    l+= data_l;
    ut_ad(end == l);
    m_log.close(l);
  }
  else
  {
    m_log.close(l);
    l= m_log.open(len - hdr_l - data_l);
    ut_d(const byte * const end = l + len - hdr_l - data_l);
    *l++= reuse ? INSERT_REUSE_DYNAMIC : INSERT_HEAP_DYNAMIC;
    l= mlog_encode_varint(l, prev_rec);
    if (reuse)
      l= mlog_encode_varint(l, shift);
    l= mlog_encode_varint(l, enc_hdr_l);
    l= mlog_encode_varint(l, hdr_c);
    l= mlog_encode_varint(l, data_c);
    ut_ad(end == l);
    m_log.close(l);
    m_log.push(hdr, static_cast<uint32_t>(hdr_l));
    m_log.push(data, static_cast<uint32_t>(data_l));
  }

  m_last_offset= FIL_PAGE_TYPE;
}

/** Report page directory corruption.
@param block  index page
@param index  index tree
*/
ATTRIBUTE_COLD
static void page_cur_directory_corrupted(const buf_block_t &block,
                                         const dict_index_t &index)
{
  ib::error() << "Directory of " << block.page.id()
              << " of index " << index.name
              << " in table " << index.table->name
              << " is corrupted";
}

/***********************************************************//**
Inserts a record next to page cursor on an uncompressed page.
@return pointer to record
@retval nullptr if not enough space was available */
rec_t*
page_cur_insert_rec_low(
/*====================*/
	const page_cur_t*cur,	/*!< in: page cursor */
	const rec_t*	rec,	/*!< in: record to insert after cur */
	rec_offs*	offsets,/*!< in/out: rec_get_offsets(rec, index) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
  buf_block_t *block= cur->block;
  dict_index_t * const index= cur->index;

  ut_ad(rec_offs_validate(rec, index, offsets));
  ut_ad(rec_offs_n_fields(offsets) > 0);
  ut_ad(index->table->not_redundant() == !!page_is_comp(block->page.frame));
  ut_ad(!!page_is_comp(block->page.frame) == !!rec_offs_comp(offsets));
  ut_ad(fil_page_index_page_check(block->page.frame));
  ut_ad(mach_read_from_8(PAGE_HEADER + PAGE_INDEX_ID + block->page.frame) ==
        index->id || index->is_dummy);
  ut_ad(page_dir_get_n_slots(block->page.frame) >= 2);

  ut_ad(!page_rec_is_supremum(cur->rec));

  /* We should not write log for ROW_FORMAT=COMPRESSED pages here. */
  ut_ad(!mtr->is_logged() ||
        !(index->table->flags & DICT_TF_MASK_ZIP_SSIZE));

  /* 1. Get the size of the physical record in the page */
  const ulint rec_size= rec_offs_size(offsets);

#ifdef HAVE_MEM_CHECK
  {
    const void *rec_start __attribute__((unused))=
      rec - rec_offs_extra_size(offsets);
    ulint extra_size __attribute__((unused))=
      rec_offs_extra_size(offsets) -
      (page_is_comp(block->page.frame)
       ? REC_N_NEW_EXTRA_BYTES
       : REC_N_OLD_EXTRA_BYTES);
    /* All data bytes of the record must be valid. */
    MEM_CHECK_DEFINED(rec, rec_offs_data_size(offsets));
    /* The variable-length header must be valid. */
    MEM_CHECK_DEFINED(rec_start, extra_size);
  }
#endif /* HAVE_MEM_CHECK */

  /* 2. Try to find suitable space from page memory management */
  bool reuse= false;
  ssize_t free_offset= 0;
  ulint heap_no;
  byte *insert_buf;

  const bool comp= page_is_comp(block->page.frame);
  const ulint extra_size= rec_offs_extra_size(offsets);

  if (rec_t* free_rec= page_header_get_ptr(block->page.frame, PAGE_FREE))
  {
    /* Try to reuse the head of PAGE_FREE. */
    rec_offs foffsets_[REC_OFFS_NORMAL_SIZE];
    mem_heap_t *heap= nullptr;

    rec_offs_init(foffsets_);

    rec_offs *foffsets= rec_get_offsets(free_rec, index, foffsets_,
                                        page_is_leaf(block->page.frame)
                                        ? index->n_core_fields : 0,
                                        ULINT_UNDEFINED, &heap);
    const ulint fextra_size= rec_offs_extra_size(foffsets);
    insert_buf= free_rec - fextra_size;
    const bool too_small= (fextra_size + rec_offs_data_size(foffsets)) <
      rec_size;
    if (UNIV_LIKELY_NULL(heap))
      mem_heap_free(heap);

    if (too_small)
      goto use_heap;

    byte *page_free= my_assume_aligned<2>(PAGE_FREE + PAGE_HEADER +
                                          block->page.frame);
    if (comp)
    {
      heap_no= rec_get_heap_no_new(free_rec);
      uint16_t next= mach_read_from_2(free_rec - REC_NEXT);
      mach_write_to_2(page_free, next
                      ? static_cast<uint16_t>(free_rec + next -
                                              block->page.frame)
                      : 0);
    }
    else
    {
      heap_no= rec_get_heap_no_old(free_rec);
      memcpy(page_free, free_rec - REC_NEXT, 2);
    }

    static_assert(PAGE_GARBAGE == PAGE_FREE + 2, "compatibility");

    byte *page_garbage= my_assume_aligned<2>(page_free + 2);
    ut_ad(mach_read_from_2(page_garbage) >= rec_size);
    mach_write_to_2(page_garbage, mach_read_from_2(page_garbage) - rec_size);
    reuse= true;
    free_offset= extra_size - fextra_size;
  }
  else
  {
use_heap:
    insert_buf= page_mem_alloc_heap(block, rec_size, &heap_no);

    if (UNIV_UNLIKELY(!insert_buf))
      return nullptr;
  }

  ut_ad(cur->rec != insert_buf + extra_size);

  rec_t *next_rec= block->page.frame + rec_get_next_offs(cur->rec, comp);
  ut_ad(next_rec != block->page.frame);

  /* Update page header fields */
  byte *page_last_insert= my_assume_aligned<2>(PAGE_LAST_INSERT + PAGE_HEADER +
                                               block->page.frame);
  const uint16_t last_insert= mach_read_from_2(page_last_insert);
  ut_ad(!last_insert || !comp ||
        rec_get_node_ptr_flag(block->page.frame + last_insert) ==
        rec_get_node_ptr_flag(rec));

  /* Write PAGE_LAST_INSERT */
  mach_write_to_2(page_last_insert,
                  insert_buf + extra_size - block->page.frame);

  /* Update PAGE_DIRECTION_B, PAGE_N_DIRECTION if needed */
  if (block->page.frame[FIL_PAGE_TYPE + 1] != byte(FIL_PAGE_RTREE))
  {
    byte *dir= &block->page.frame[PAGE_DIRECTION_B + PAGE_HEADER];
    byte *n= my_assume_aligned<2>
      (&block->page.frame[PAGE_N_DIRECTION + PAGE_HEADER]);
    if (UNIV_UNLIKELY(!last_insert))
    {
no_direction:
      *dir= static_cast<byte>((*dir & ~((1U << 3) - 1)) | PAGE_NO_DIRECTION);
      memset(n, 0, 2);
    }
    else if (block->page.frame + last_insert == cur->rec &&
             (*dir & ((1U << 3) - 1)) != PAGE_LEFT)
    {
      *dir= static_cast<byte>((*dir & ~((1U << 3) - 1)) | PAGE_RIGHT);
inc_dir:
      mach_write_to_2(n, mach_read_from_2(n) + 1);
    }
    else if (next_rec == block->page.frame + last_insert &&
             (*dir & ((1U << 3) - 1)) != PAGE_RIGHT)
    {
      *dir= static_cast<byte>((*dir & ~((1U << 3) - 1)) | PAGE_LEFT);
      goto inc_dir;
    }
    else
      goto no_direction;
  }

  /* Update PAGE_N_RECS. */
  byte *page_n_recs= my_assume_aligned<2>(PAGE_N_RECS + PAGE_HEADER +
                                          block->page.frame);

  mach_write_to_2(page_n_recs, mach_read_from_2(page_n_recs) + 1);

  /* Update the preceding record header, the 'owner' record and
  prepare the record to insert. */
  rec_t *insert_rec= insert_buf + extra_size;
  const ulint data_size= rec_offs_data_size(offsets);
  memcpy(insert_buf, rec - extra_size, extra_size + data_size);
  size_t hdr_common= 0;
  ulint n_owned;
  const byte info_status= static_cast<byte>
    (rec_get_info_and_status_bits(rec, comp));
  ut_ad(!(rec_get_info_bits(rec, comp) &
          ~(REC_INFO_DELETED_FLAG | REC_INFO_MIN_REC_FLAG)));

  if (comp)
  {
#ifdef UNIV_DEBUG
    switch (rec_get_status(cur->rec)) {
    case REC_STATUS_ORDINARY:
    case REC_STATUS_NODE_PTR:
    case REC_STATUS_INSTANT:
    case REC_STATUS_INFIMUM:
      break;
    case REC_STATUS_SUPREMUM:
      ut_ad("wrong status on cur->rec" == 0);
    }
    switch (rec_get_status(rec)) {
    case REC_STATUS_NODE_PTR:
      ut_ad(!page_is_leaf(block->page.frame));
      break;
    case REC_STATUS_INSTANT:
      ut_ad(index->is_instant());
      ut_ad(page_is_leaf(block->page.frame));
      if (!rec_is_metadata(rec, true))
        break;
      ut_ad(cur->rec == &block->page.frame[PAGE_NEW_INFIMUM]);
      break;
    case REC_STATUS_ORDINARY:
      ut_ad(page_is_leaf(block->page.frame));
      ut_ad(!(rec_get_info_bits(rec, true) & ~REC_INFO_DELETED_FLAG));
      break;
    case REC_STATUS_INFIMUM:
    case REC_STATUS_SUPREMUM:
      ut_ad("wrong status on rec" == 0);
    }
    ut_ad(rec_get_status(next_rec) != REC_STATUS_INFIMUM);
#endif

    rec_set_bit_field_1(insert_rec, 0, REC_NEW_N_OWNED,
                        REC_N_OWNED_MASK, REC_N_OWNED_SHIFT);
    insert_rec[-REC_NEW_STATUS]= rec[-REC_NEW_STATUS];
    rec_set_bit_field_2(insert_rec, heap_no,
                        REC_NEW_HEAP_NO, REC_HEAP_NO_MASK, REC_HEAP_NO_SHIFT);
    mach_write_to_2(insert_rec - REC_NEXT,
                    static_cast<uint16_t>(next_rec - insert_rec));
    mach_write_to_2(cur->rec - REC_NEXT,
                    static_cast<uint16_t>(insert_rec - cur->rec));
    while (!(n_owned= rec_get_n_owned_new(next_rec)))
    {
      next_rec= block->page.frame + rec_get_next_offs(next_rec, true);
      ut_ad(next_rec != block->page.frame);
    }
    rec_set_bit_field_1(next_rec, n_owned + 1, REC_NEW_N_OWNED,
                        REC_N_OWNED_MASK, REC_N_OWNED_SHIFT);
    if (!mtr->is_logged())
    {
      mtr->set_modified(*block);
      goto copied;
    }

    const byte * const c_start= cur->rec - extra_size;
    if (extra_size > REC_N_NEW_EXTRA_BYTES &&
        c_start >=
        &block->page.frame[PAGE_NEW_SUPREMUM_END + REC_N_NEW_EXTRA_BYTES])
    {
      /* Find common header bytes with the preceding record. */
      const byte *r= rec - (REC_N_NEW_EXTRA_BYTES + 1);
      for (const byte *c= cur->rec - (REC_N_NEW_EXTRA_BYTES + 1);
           *r == *c && c-- != c_start; r--);
      hdr_common= static_cast<size_t>((rec - (REC_N_NEW_EXTRA_BYTES + 1)) - r);
      ut_ad(hdr_common <= extra_size - REC_N_NEW_EXTRA_BYTES);
    }
  }
  else
  {
#ifdef UNIV_DEBUG
    if (!page_is_leaf(block->page.frame));
    else if (rec_is_metadata(rec, false))
    {
      ut_ad(index->is_instant());
      ut_ad(cur->rec == &block->page.frame[PAGE_OLD_INFIMUM]);
    }
#endif
    rec_set_bit_field_1(insert_rec, 0, REC_OLD_N_OWNED,
                        REC_N_OWNED_MASK, REC_N_OWNED_SHIFT);
    rec_set_bit_field_2(insert_rec, heap_no,
                        REC_OLD_HEAP_NO, REC_HEAP_NO_MASK, REC_HEAP_NO_SHIFT);
    memcpy(insert_rec - REC_NEXT, cur->rec - REC_NEXT, 2);
    mach_write_to_2(cur->rec - REC_NEXT, insert_rec - block->page.frame);
    while (!(n_owned= rec_get_n_owned_old(next_rec)))
    {
      next_rec= block->page.frame + rec_get_next_offs(next_rec, false);
      ut_ad(next_rec != block->page.frame);
    }
    rec_set_bit_field_1(next_rec, n_owned + 1, REC_OLD_N_OWNED,
                        REC_N_OWNED_MASK, REC_N_OWNED_SHIFT);
    if (!mtr->is_logged())
    {
      mtr->set_modified(*block);
      goto copied;
    }

    ut_ad(extra_size > REC_N_OLD_EXTRA_BYTES);
    const byte * const c_start= cur->rec - extra_size;
    if (c_start >=
        &block->page.frame[PAGE_OLD_SUPREMUM_END + REC_N_OLD_EXTRA_BYTES])
    {
      /* Find common header bytes with the preceding record. */
      const byte *r= rec - (REC_N_OLD_EXTRA_BYTES + 1);
      for (const byte *c= cur->rec - (REC_N_OLD_EXTRA_BYTES + 1);
           *r == *c && c-- != c_start; r--);
      hdr_common= static_cast<size_t>((rec - (REC_N_OLD_EXTRA_BYTES + 1)) - r);
      ut_ad(hdr_common <= extra_size - REC_N_OLD_EXTRA_BYTES);
    }
  }

  /* Insert the record, possibly copying from the preceding record. */
  ut_ad(mtr->is_logged());

  {
    const byte *r= rec;
    const byte *c= cur->rec;
    const byte *c_end= c + data_size;
    if (page_rec_is_infimum(c) && data_size > 8)
      c_end= c + 8;
    static_assert(REC_N_OLD_EXTRA_BYTES == REC_N_NEW_EXTRA_BYTES + 1, "");
    if (c <= insert_buf && c_end > insert_buf)
      c_end= insert_buf;
    else if (c_end < next_rec &&
             c_end >= next_rec - REC_N_OLD_EXTRA_BYTES + comp)
      c_end= next_rec - REC_N_OLD_EXTRA_BYTES + comp;
    else
      c_end= std::min<const byte*>(c_end, block->page.frame + srv_page_size -
                                   PAGE_DIR - PAGE_DIR_SLOT_SIZE *
                                   page_dir_get_n_slots(block->page.frame));
    size_t data_common;
    /* Copy common data bytes of the preceding record. */
    for (; c != c_end && *r == *c; c++, r++);
    data_common= static_cast<size_t>(r - rec);

    if (comp)
      mtr->page_insert(*block, reuse,
                       cur->rec - block->page.frame - PAGE_NEW_INFIMUM,
                       info_status, free_offset, hdr_common, data_common,
                       insert_buf,
                       extra_size - hdr_common - REC_N_NEW_EXTRA_BYTES,
                       r, data_size - data_common);
    else
      mtr->page_insert(*block, reuse,
                       cur->rec - block->page.frame - PAGE_OLD_INFIMUM,
                       info_status, rec_get_n_fields_old(insert_rec) << 1 |
                       rec_get_1byte_offs_flag(insert_rec),
                       hdr_common, data_common,
                       insert_buf,
                       extra_size - hdr_common - REC_N_OLD_EXTRA_BYTES,
                       r, data_size - data_common);
  }

copied:
  ut_ad(!memcmp(insert_buf, rec - extra_size, extra_size -
                (comp ? REC_N_NEW_EXTRA_BYTES : REC_N_OLD_EXTRA_BYTES)));
  ut_ad(!memcmp(insert_rec, rec, data_size));
  /* We have incremented the n_owned field of the owner record.
  If the number exceeds PAGE_DIR_SLOT_MAX_N_OWNED, we have to split the
  corresponding directory slot in two. */

  if (UNIV_UNLIKELY(n_owned == PAGE_DIR_SLOT_MAX_N_OWNED))
  {
    const ulint owner= page_dir_find_owner_slot(next_rec);
    if (UNIV_UNLIKELY(owner == ULINT_UNDEFINED))
    {
      page_cur_directory_corrupted(*block, *index);
      return nullptr;
    }

    if (page_dir_split_slot(*block, page_dir_get_nth_slot(block->page.frame,
                                                          owner)))
      return nullptr;
  }

  rec_offs_make_valid(insert_buf + extra_size, index,
                      page_is_leaf(block->page.frame), offsets);
  return insert_buf + extra_size;
}

/** Add a slot to the dense page directory.
@param[in,out]  block   ROW_FORMAT=COMPRESSED page
@param[in]      index   the index that the page belongs to
@param[in,out]  mtr     mini-transaction */
static inline void page_zip_dir_add_slot(buf_block_t *block,
                                         const dict_index_t *index, mtr_t *mtr)
{
  page_zip_des_t *page_zip= &block->page.zip;

  ut_ad(page_is_comp(page_zip->data));
  MEM_CHECK_DEFINED(page_zip->data, page_zip_get_size(page_zip));

  /* Read the old n_dense (n_heap has already been incremented). */
  ulint n_dense= page_dir_get_n_heap(page_zip->data) - (PAGE_HEAP_NO_USER_LOW +
                                                        1U);

  byte *dir= page_zip->data + page_zip_get_size(page_zip) -
    PAGE_ZIP_DIR_SLOT_SIZE * n_dense;
  byte *stored= dir;

  if (!page_is_leaf(page_zip->data))
  {
    ut_ad(!page_zip->n_blobs);
    stored-= n_dense * REC_NODE_PTR_SIZE;
  }
  else if (index->is_clust())
  {
    /* Move the BLOB pointer array backwards to make space for the
    columns DB_TRX_ID,DB_ROLL_PTR and the dense directory slot. */

    stored-= n_dense * (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);
    byte *externs= stored - page_zip->n_blobs * BTR_EXTERN_FIELD_REF_SIZE;
    byte *dst= externs - PAGE_ZIP_CLUST_LEAF_SLOT_SIZE;
    ut_ad(!memcmp(dst, field_ref_zero, PAGE_ZIP_CLUST_LEAF_SLOT_SIZE));
    if (const ulint len = ulint(stored - externs))
    {
      memmove(dst, externs, len);
      mtr->memmove(*block, dst - page_zip->data, externs - page_zip->data,
                   len);
    }
  }
  else
  {
    stored-= page_zip->n_blobs * BTR_EXTERN_FIELD_REF_SIZE;
    ut_ad(!memcmp(stored - PAGE_ZIP_DIR_SLOT_SIZE, field_ref_zero,
                  PAGE_ZIP_DIR_SLOT_SIZE));
  }

  /* Move the uncompressed area backwards to make space
  for one directory slot. */
  if (const ulint len = ulint(dir - stored))
  {
    byte* dst = stored - PAGE_ZIP_DIR_SLOT_SIZE;
    memmove(dst, stored, len);
    mtr->memmove(*block, dst - page_zip->data, stored - page_zip->data, len);
  }
}

/***********************************************************//**
Inserts a record next to page cursor on a compressed and uncompressed
page.

@return pointer to inserted record
@return nullptr on failure */
rec_t*
page_cur_insert_rec_zip(
/*====================*/
	page_cur_t*	cursor,	/*!< in/out: page cursor,
				logical position unchanged  */
	const rec_t*	rec,	/*!< in: pointer to a physical record */
	rec_offs*	offsets,/*!< in/out: rec_get_offsets(rec, index) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
  page_zip_des_t * const page_zip= page_cur_get_page_zip(cursor);
  page_t * const page= cursor->block->page.frame;
  dict_index_t * const index = cursor->index;

  ut_ad(page_zip);
  ut_ad(rec_offs_validate(rec, index, offsets));

  ut_ad(index->table->not_redundant());
  ut_ad(page_is_comp(page));
  ut_ad(rec_offs_comp(offsets));
  ut_ad(fil_page_get_type(page) == FIL_PAGE_INDEX ||
        fil_page_get_type(page) == FIL_PAGE_RTREE);
  ut_ad(mach_read_from_8(PAGE_HEADER + PAGE_INDEX_ID + page) == index->id ||
        index->is_dummy);
  ut_ad(!page_get_instant(page));
  ut_ad(!page_cur_is_after_last(cursor));
#ifdef UNIV_ZIP_DEBUG
  ut_a(page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */

  /* 1. Get the size of the physical record in the page */
  const ulint rec_size= rec_offs_size(offsets);

#ifdef HAVE_MEM_CHECK
  {
    const void *rec_start __attribute__((unused))=
      rec - rec_offs_extra_size(offsets);
    ulint extra_size __attribute__((unused))=
      rec_offs_extra_size(offsets) - REC_N_NEW_EXTRA_BYTES;
    /* All data bytes of the record must be valid. */
    MEM_CHECK_DEFINED(rec, rec_offs_data_size(offsets));
    /* The variable-length header must be valid. */
    MEM_CHECK_DEFINED(rec_start, extra_size);
  }
#endif /* HAVE_MEM_CHECK */
  const bool reorg_before_insert= page_has_garbage(page) &&
    rec_size > page_get_max_insert_size(page, 1) &&
    rec_size <= page_get_max_insert_size_after_reorganize(page, 1);
  constexpr uint16_t page_free_f= PAGE_FREE + PAGE_HEADER;
  byte* const page_free = my_assume_aligned<4>(page_free_f + page);
  uint16_t free_rec= 0;

  /* 2. Try to find suitable space from page memory management */
  ulint heap_no;
  byte *insert_buf;

  if (reorg_before_insert ||
      !page_zip_available(page_zip, index->is_clust(), rec_size, 1))
  {
    /* SET GLOBAL might be executed concurrently. Sample the value once. */
    ulint level= page_zip_level;
#ifdef UNIV_DEBUG
    const rec_t * const cursor_rec= page_cur_get_rec(cursor);
#endif /* UNIV_DEBUG */

    if (page_is_empty(page))
    {
      ut_ad(page_cur_is_before_first(cursor));

      /* This is an empty page. Recreate to remove the modification log. */
      page_create_zip(cursor->block, index,
                      page_header_get_field(page, PAGE_LEVEL), 0, mtr);
      ut_ad(!page_header_get_ptr(page, PAGE_FREE));

      if (page_zip_available(page_zip, index->is_clust(), rec_size, 1))
        goto use_heap;

      /* The cursor should remain on the page infimum. */
      return nullptr;
    }

    if (page_zip->m_nonempty || page_has_garbage(page))
    {
      ulint pos= page_rec_get_n_recs_before(cursor->rec);

      if (UNIV_UNLIKELY(pos == ULINT_UNDEFINED))
        return nullptr;

      switch (page_zip_reorganize(cursor->block, index, level, mtr, true)) {
      case DB_FAIL:
        ut_ad(cursor->rec == cursor_rec);
        return nullptr;
      case DB_SUCCESS:
        break;
      default:
        return nullptr;
      }

      if (!pos)
        ut_ad(cursor->rec == page + PAGE_NEW_INFIMUM);
      else if (!(cursor->rec= page_rec_get_nth(page, pos)))
      {
        cursor->rec= page + PAGE_NEW_SUPREMUM;
        return nullptr;
      }

      ut_ad(!page_header_get_ptr(page, PAGE_FREE));

      if (page_zip_available(page_zip, index->is_clust(), rec_size, 1))
        goto use_heap;
    }

    /* Try compressing the whole page afterwards. */
    const mtr_log_t log_mode= mtr->set_log_mode(MTR_LOG_NONE);
    rec_t *insert_rec= page_cur_insert_rec_low(cursor, rec, offsets, mtr);
    mtr->set_log_mode(log_mode);

    if (insert_rec)
    {
      ulint pos= page_rec_get_n_recs_before(insert_rec);
      if (UNIV_UNLIKELY(!pos || pos == ULINT_UNDEFINED))
        return nullptr;

      /* We are writing entire page images to the log.  Reduce the redo
      log volume by reorganizing the page at the same time. */
      switch (page_zip_reorganize(cursor->block, index, level, mtr)) {
      case DB_SUCCESS:
        /* The page was reorganized: Seek to pos. */
        if (pos <= 1)
          cursor->rec= page + PAGE_NEW_INFIMUM;
        else if (!(cursor->rec= page_rec_get_nth(page, pos - 1)))
        {
          cursor->rec= page + PAGE_NEW_INFIMUM;
          return nullptr;
        }
        insert_rec= page + rec_get_next_offs(cursor->rec, 1);
        rec_offs_make_valid(insert_rec, index, page_is_leaf(page), offsets);
        break;
      case DB_FAIL:
        /* Theoretically, we could try one last resort of
           page_zip_reorganize() followed by page_zip_available(), but that
           would be very unlikely to succeed. (If the full reorganized page
           failed to compress, why would it succeed to compress the page,
           plus log the insert of this record?) */

        /* Out of space: restore the page */
        if (!page_zip_decompress(page_zip, page, false))
          ut_error; /* Memory corrupted? */
        ut_ad(page_validate(page, index));
        /* fall through */
      default:
        insert_rec= nullptr;
      }
    }
    return insert_rec;
  }

  free_rec= mach_read_from_2(page_free);
  if (free_rec)
  {
    /* Try to allocate from the head of the free list. */
    rec_offs foffsets_[REC_OFFS_NORMAL_SIZE];
    mem_heap_t *heap= nullptr;

    rec_offs_init(foffsets_);

    rec_offs *foffsets= rec_get_offsets(page + free_rec, index, foffsets_,
                                        page_is_leaf(page)
                                        ? index->n_core_fields : 0,
                                        ULINT_UNDEFINED, &heap);
    insert_buf= page + free_rec - rec_offs_extra_size(foffsets);

    if (rec_offs_size(foffsets) < rec_size)
    {
too_small:
      if (UNIV_LIKELY_NULL(heap))
        mem_heap_free(heap);
      free_rec= 0;
      goto use_heap;
    }

    /* On compressed pages, do not relocate records from
    the free list. If extra_size would grow, use the heap. */
    const ssize_t extra_size_diff= lint(rec_offs_extra_size(offsets) -
                                        rec_offs_extra_size(foffsets));

    if (UNIV_UNLIKELY(extra_size_diff < 0))
    {
      /* Add an offset to the extra_size. */
      if (rec_offs_size(foffsets) < rec_size - ssize_t(extra_size_diff))
        goto too_small;

      insert_buf-= extra_size_diff;
    }
    else if (UNIV_UNLIKELY(extra_size_diff))
      /* Do not allow extra_size to grow */
      goto too_small;

    byte *const free_rec_ptr= page + free_rec;
    heap_no= rec_get_heap_no_new(free_rec_ptr);
    int16_t next_free= mach_read_from_2(free_rec_ptr - REC_NEXT);
    /* With innodb_page_size=64k, int16_t would be unsafe to use here,
    but that cannot be used with ROW_FORMAT=COMPRESSED. */
    static_assert(UNIV_ZIP_SIZE_SHIFT_MAX == 14, "compatibility");
    if (next_free)
    {
      next_free= static_cast<int16_t>(next_free + free_rec);
      if (UNIV_UNLIKELY(int{PAGE_NEW_SUPREMUM_END + REC_N_NEW_EXTRA_BYTES} >
                        next_free ||
                        uint16_t(next_free) >= srv_page_size))
      {
        if (UNIV_LIKELY_NULL(heap))
          mem_heap_free(heap);
        return nullptr;
      }
    }

    byte *hdr= my_assume_aligned<4>(&page_zip->data[page_free_f]);
    mach_write_to_2(hdr, static_cast<uint16_t>(next_free));
    const byte *const garbage= my_assume_aligned<2>(page_free + 2);
    ut_ad(mach_read_from_2(garbage) >= rec_size);
    mach_write_to_2(my_assume_aligned<2>(hdr + 2),
                    mach_read_from_2(garbage) - rec_size);
    static_assert(PAGE_GARBAGE == PAGE_FREE + 2, "compatibility");
    mtr->memcpy(*cursor->block, page_free, hdr, 4);

    if (!page_is_leaf(page))
    {
      /* Zero out the node pointer of free_rec, in case it will not be
      overwritten by insert_rec. */
      ut_ad(rec_size > REC_NODE_PTR_SIZE);

      if (rec_offs_size(foffsets) > rec_size)
        memset(rec_get_end(free_rec_ptr, foffsets) -
               REC_NODE_PTR_SIZE, 0, REC_NODE_PTR_SIZE);
    }
    else if (index->is_clust())
    {
      /* Zero out DB_TRX_ID,DB_ROLL_PTR in free_rec, in case they will
      not be overwritten by insert_rec. */

      ulint len;
      ulint trx_id_offs= rec_get_nth_field_offs(foffsets, index->db_trx_id(),
                                                &len);
      ut_ad(len == DATA_TRX_ID_LEN);

      if (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN + trx_id_offs +
          rec_offs_extra_size(foffsets) > rec_size)
        memset(free_rec_ptr + trx_id_offs, 0,
               DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);

      ut_ad(free_rec_ptr + trx_id_offs + DATA_TRX_ID_LEN ==
            rec_get_nth_field(free_rec_ptr, foffsets, index->db_roll_ptr(),
                              &len));
      ut_ad(len == DATA_ROLL_PTR_LEN);
    }

    if (UNIV_LIKELY_NULL(heap))
      mem_heap_free(heap);
  }
  else
  {
use_heap:
    ut_ad(!free_rec);
    insert_buf= page_mem_alloc_heap<true>(cursor->block, rec_size, &heap_no);

    if (UNIV_UNLIKELY(!insert_buf))
      return insert_buf;

    static_assert(PAGE_N_HEAP == PAGE_HEAP_TOP + 2, "compatibility");
    mtr->memcpy(*cursor->block, PAGE_HEAP_TOP + PAGE_HEADER, 4);
    page_zip_dir_add_slot(cursor->block, index, mtr);
  }

  /* next record after current before the insertion */
  const rec_t *next_rec = page_rec_next_get<true>(page, cursor->rec);
  if (UNIV_UNLIKELY(!next_rec ||
                    rec_get_status(next_rec) == REC_STATUS_INFIMUM ||
                    rec_get_status(cursor->rec) > REC_STATUS_INFIMUM))
    return nullptr;

  /* 3. Create the record */
  byte *insert_rec= rec_copy(insert_buf, rec, offsets);
  rec_offs_make_valid(insert_rec, index, page_is_leaf(page), offsets);

  /* 4. Insert the record in the linked list of records */
  ut_ad(cursor->rec != insert_rec);
  ut_ad(rec_get_status(insert_rec) < REC_STATUS_INFIMUM);

  mach_write_to_2(insert_rec - REC_NEXT, static_cast<uint16_t>
                  (next_rec - insert_rec));
  mach_write_to_2(cursor->rec - REC_NEXT, static_cast<uint16_t>
                  (insert_rec - cursor->rec));
  byte *n_recs= my_assume_aligned<2>(PAGE_N_RECS + PAGE_HEADER + page);
  mtr->write<2>(*cursor->block, n_recs, 1U + mach_read_from_2(n_recs));
  memcpy_aligned<2>(&page_zip->data[PAGE_N_RECS + PAGE_HEADER], n_recs, 2);

  /* 5. Set the n_owned field in the inserted record to zero,
  and set the heap_no field */
  rec_set_bit_field_1(insert_rec, 0, REC_NEW_N_OWNED,
                      REC_N_OWNED_MASK, REC_N_OWNED_SHIFT);
  rec_set_bit_field_2(insert_rec, heap_no, REC_NEW_HEAP_NO,
                      REC_HEAP_NO_MASK, REC_HEAP_NO_SHIFT);

  MEM_CHECK_DEFINED(rec_get_start(insert_rec, offsets),
                    rec_offs_size(offsets));

  /* 6. Update the last insertion info in page header */
  byte *last_insert= my_assume_aligned<4>(PAGE_LAST_INSERT + PAGE_HEADER +
                                          page_zip->data);
  const uint16_t last_insert_rec= mach_read_from_2(last_insert);
  ut_ad(!last_insert_rec ||
        rec_get_node_ptr_flag(page + last_insert_rec) ==
        rec_get_node_ptr_flag(insert_rec));
  mach_write_to_2(last_insert, insert_rec - page);

  if (!index->is_spatial())
  {
    byte *dir= &page_zip->data[PAGE_HEADER + PAGE_DIRECTION_B];
    ut_ad(!(*dir & ~((1U << 3) - 1)));
    byte *n= my_assume_aligned<2>
      (&page_zip->data[PAGE_HEADER + PAGE_N_DIRECTION]);
    if (UNIV_UNLIKELY(!last_insert_rec))
    {
no_direction:
      *dir= PAGE_NO_DIRECTION;
      memset(n, 0, 2);
    }
    else if (*dir != PAGE_LEFT && page + last_insert_rec == cursor->rec)
    {
      *dir= PAGE_RIGHT;
inc_dir:
      mach_write_to_2(n, mach_read_from_2(n) + 1);
    }
    else if (*dir != PAGE_RIGHT && page_rec_next_get<true>(page, insert_rec) ==
             page + last_insert_rec)
    {
      *dir= PAGE_LEFT;
      goto inc_dir;
    }
    else
      goto no_direction;
  }

  /* Write the header fields in one record. */
  mtr->memcpy(*cursor->block,
              my_assume_aligned<8>(PAGE_LAST_INSERT + PAGE_HEADER + page),
              my_assume_aligned<8>(PAGE_LAST_INSERT + PAGE_HEADER +
                                   page_zip->data),
              PAGE_N_RECS - PAGE_LAST_INSERT + 2);

  /* 7. It remains to update the owner record. */
  ulint n_owned;

  while (!(n_owned= rec_get_n_owned_new(next_rec)))
    if (!(next_rec= page_rec_next_get<true>(page, next_rec)))
      return nullptr;

  rec_set_bit_field_1(const_cast<rec_t*>(next_rec), n_owned + 1,
                      REC_NEW_N_OWNED, REC_N_OWNED_MASK, REC_N_OWNED_SHIFT);

  page_zip_dir_insert(cursor, free_rec, insert_rec, mtr);

  /* 8. Now we have incremented the n_owned field of the owner
  record. If the number exceeds PAGE_DIR_SLOT_MAX_N_OWNED,
  we have to split the corresponding directory slot in two. */
  if (UNIV_UNLIKELY(n_owned == PAGE_DIR_SLOT_MAX_N_OWNED))
  {
    const ulint owner= page_dir_find_owner_slot(next_rec);
    if (UNIV_UNLIKELY(owner == ULINT_UNDEFINED))
    {
      page_cur_directory_corrupted(*cursor->block, *index);
      return nullptr;
    }
    page_zip_dir_split_slot(cursor->block, owner, mtr);
  }

  page_zip_write_rec(cursor->block, insert_rec, index, offsets, 1, mtr);
  return insert_rec;
}

/** Prepend a record to the PAGE_FREE list, or shrink PAGE_HEAP_TOP.
@param[in,out]  block        index page
@param[in,out]  rec          record being deleted
@param[in]      data_size    record payload size, in bytes
@param[in]      extra_size   record header size, in bytes */
static void page_mem_free(const buf_block_t &block, rec_t *rec,
                          size_t data_size, size_t extra_size)
{
  ut_ad(page_align(rec) == block.page.frame);
  ut_ad(!block.page.zip.data);
  const rec_t *free= page_header_get_ptr(block.page.frame, PAGE_FREE);

  const uint16_t n_heap= uint16_t(page_header_get_field(block.page.frame,
                                                        PAGE_N_HEAP) - 1);
  ut_ad(page_get_n_recs(block.page.frame) < (n_heap & 0x7fff));
  const bool deleting_top= n_heap == ((n_heap & 0x8000)
                                      ? (rec_get_heap_no_new(rec) | 0x8000)
                                      : rec_get_heap_no_old(rec));

  if (deleting_top)
  {
    byte *page_heap_top= my_assume_aligned<2>(PAGE_HEAP_TOP + PAGE_HEADER +
                                              block.page.frame);
    const uint16_t heap_top= mach_read_from_2(page_heap_top);
    const size_t extra_savings= heap_top -
      (rec + data_size - block.page.frame);
    ut_ad(extra_savings < heap_top);

    /* When deleting the last record, do not add it to the PAGE_FREE list.
    Instead, decrement PAGE_HEAP_TOP and PAGE_N_HEAP. */
    mach_write_to_2(page_heap_top, rec - extra_size - block.page.frame);
    mach_write_to_2(my_assume_aligned<2>(page_heap_top + 2), n_heap);
    static_assert(PAGE_N_HEAP == PAGE_HEAP_TOP + 2, "compatibility");
    if (extra_savings)
    {
      byte *page_garbage= my_assume_aligned<2>(PAGE_GARBAGE + PAGE_HEADER +
                                               block.page.frame);
      uint16_t garbage= mach_read_from_2(page_garbage);
      ut_ad(garbage >= extra_savings);
      mach_write_to_2(page_garbage, garbage - extra_savings);
    }
  }
  else
  {
    byte *page_free= my_assume_aligned<2>(PAGE_FREE + PAGE_HEADER +
                                          block.page.frame);
    byte *page_garbage= my_assume_aligned<2>(PAGE_GARBAGE + PAGE_HEADER +
                                             block.page.frame);
    mach_write_to_2(page_free, rec - block.page.frame);
    mach_write_to_2(page_garbage, mach_read_from_2(page_garbage) +
                    extra_size + data_size);
  }

  memset_aligned<2>(PAGE_LAST_INSERT + PAGE_HEADER + block.page.frame, 0, 2);
  byte *page_n_recs= my_assume_aligned<2>(PAGE_N_RECS + PAGE_HEADER +
                                          block.page.frame);
  mach_write_to_2(page_n_recs, mach_read_from_2(page_n_recs) - 1);

  const byte* const end= rec + data_size;

  if (!deleting_top)
  {
    uint16_t next= free
      ? ((n_heap & 0x8000)
         ? static_cast<uint16_t>(free - rec)
         : static_cast<uint16_t>(free - block.page.frame))
      : uint16_t{0};
    mach_write_to_2(rec - REC_NEXT, next);
  }
  else
    rec-= extra_size;

  memset(rec, 0, end - rec);
}

/***********************************************************//**
Deletes a record at the page cursor. The cursor is moved to the next
record after the deleted one. */
void
page_cur_delete_rec(
/*================*/
	page_cur_t*		cursor,	/*!< in/out: a page cursor */
	const rec_offs*		offsets,/*!< in: rec_get_offsets(
					cursor->rec, index) */
	mtr_t*			mtr)	/*!< in/out: mini-transaction */
{
	page_dir_slot_t* cur_dir_slot;
	rec_t*		current_rec;
	rec_t*		prev_rec	= NULL;
	rec_t*		next_rec;
	ulint		cur_n_owned;
	rec_t*		rec;

	/* page_zip_validate() will fail here when
	btr_cur_pessimistic_delete() invokes btr_set_min_rec_mark().
	Then, both "page_zip" and "block->page.frame" would have the
	min-rec-mark set on the smallest user record, but
	"block->page.frame" would additionally have it set on the
	smallest-but-one record.  Because sloppy
	page_zip_validate_low() only ignores min-rec-flag differences
	in the smallest user record, it cannot be used here either. */

	current_rec = cursor->rec;
	const dict_index_t* const index = cursor->index;
	buf_block_t* const block = cursor->block;
	ut_ad(rec_offs_validate(current_rec, index, offsets));
	ut_ad(!!page_is_comp(block->page.frame)
	      == index->table->not_redundant());
	ut_ad(fil_page_index_page_check(block->page.frame));
	ut_ad(mach_read_from_8(PAGE_HEADER + PAGE_INDEX_ID + block->page.frame)
	      == index->id || index->is_dummy);
	ut_ad(mtr->is_named_space(index->table->space));

	/* The record must not be the supremum or infimum record. */
	ut_ad(page_rec_is_user_rec(current_rec));

	if (page_get_n_recs(block->page.frame) == 1
	    && !rec_is_alter_metadata(current_rec, *index)) {
		/* Empty the page. */
		ut_ad(page_is_leaf(block->page.frame));
		/* Usually, this should be the root page,
		and the whole index tree should become empty.
		However, this could also be a call in
		btr_cur_pessimistic_update() to delete the only
		record in the page and to insert another one. */
		ut_ad(page_rec_is_supremum(page_rec_get_next(cursor->rec)));
		page_cur_set_after_last(block, cursor);
		page_create_empty(page_cur_get_block(cursor),
				  const_cast<dict_index_t*>(index), mtr);
		return;
	}

	/* Save to local variables some data associated with current_rec */
	ulint cur_slot_no = page_dir_find_owner_slot(current_rec);

	if (UNIV_UNLIKELY(!cur_slot_no || cur_slot_no == ULINT_UNDEFINED)) {
		/* Avoid crashing due to a corrupted page. */
		page_cur_directory_corrupted(*block, *index);
		return;
	}

	cur_dir_slot = page_dir_get_nth_slot(block->page.frame, cur_slot_no);
	cur_n_owned = page_dir_slot_get_n_owned(cur_dir_slot);

	/* The page gets invalid for btr_pcur_restore_pos().
	We avoid invoking buf_block_modify_clock_inc(block) because its
	consistency checks would fail for the dummy block that is being
	used during IMPORT TABLESPACE. */
	block->modify_clock++;

	/* Find the next and the previous record. Note that the cursor is
	left at the next record. */

	rec = const_cast<rec_t*>
		(page_dir_slot_get_rec(block->page.frame,
				       cur_dir_slot + PAGE_DIR_SLOT_SIZE));

	/* rec now points to the record of the previous directory slot. Look
	for the immediate predecessor of current_rec in a loop. */

	while (current_rec != rec) {
		prev_rec = rec;
		if (!(rec = page_rec_get_next(rec))) {
			/* Avoid crashing due to a corrupted page. */
			return;
                }
	}

	if (!(next_rec = page_cur_move_to_next(cursor))) {
		/* Avoid crashing due to a corrupted page. */
		return;
	}

	/* Remove the record from the linked list of records */
	/* If the deleted record is pointed to by a dir slot, update the
	record pointer in slot. In the following if-clause we assume that
	prev_rec is owned by the same slot, i.e., PAGE_DIR_SLOT_MIN_N_OWNED
	>= 2. */
	/* Update the number of owned records of the slot */

	compile_time_assert(PAGE_DIR_SLOT_MIN_N_OWNED >= 2);
	ut_ad(cur_n_owned > 1);

	rec_t* slot_rec = const_cast<rec_t*>
		(page_dir_slot_get_rec(block->page.frame,
				       cur_dir_slot));

	if (UNIV_LIKELY_NULL(block->page.zip.data)) {
		ut_ad(page_is_comp(block->page.frame));
		if (current_rec == slot_rec) {
			page_zip_rec_set_owned(block, prev_rec, 1, mtr);
			page_zip_rec_set_owned(block, slot_rec, 0, mtr);
			slot_rec = prev_rec;
			mach_write_to_2(cur_dir_slot,
					slot_rec - block->page.frame);
		} else if (cur_n_owned == 1
			   && !page_rec_is_supremum(slot_rec)) {
			page_zip_rec_set_owned(block, slot_rec, 0, mtr);
		}

		mach_write_to_2(prev_rec - REC_NEXT, static_cast<uint16_t>
				(next_rec - prev_rec));
		slot_rec[-REC_NEW_N_OWNED] = static_cast<byte>(
			(slot_rec[-REC_NEW_N_OWNED] & ~REC_N_OWNED_MASK)
			| (cur_n_owned - 1) << REC_N_OWNED_SHIFT);

		page_header_reset_last_insert(block, mtr);
		page_zip_dir_delete(block, rec, index, offsets,
				    page_header_get_ptr(block->page.frame,
							PAGE_FREE),
				    mtr);
		if (cur_n_owned <= PAGE_DIR_SLOT_MIN_N_OWNED) {
			page_zip_dir_balance_slot(block, cur_slot_no, mtr);
		}
		return;
	}

	if (current_rec == slot_rec) {
		slot_rec = prev_rec;
		mach_write_to_2(cur_dir_slot, slot_rec - block->page.frame);
	}

	const size_t data_size = rec_offs_data_size(offsets);
	const size_t extra_size = rec_offs_extra_size(offsets);

	if (page_is_comp(block->page.frame)) {
		mtr->page_delete(*block, prev_rec - block->page.frame
				 - PAGE_NEW_INFIMUM,
				 extra_size - REC_N_NEW_EXTRA_BYTES,
				 data_size);
		mach_write_to_2(prev_rec - REC_NEXT, static_cast<uint16_t>
				(next_rec - prev_rec));
		slot_rec[-REC_NEW_N_OWNED] = static_cast<byte>(
			(slot_rec[-REC_NEW_N_OWNED] & ~REC_N_OWNED_MASK)
			| (cur_n_owned - 1) << REC_N_OWNED_SHIFT);
	} else {
		mtr->page_delete(*block, prev_rec - block->page.frame
				 - PAGE_OLD_INFIMUM);
		memcpy(prev_rec - REC_NEXT, current_rec - REC_NEXT, 2);
		slot_rec[-REC_OLD_N_OWNED] = static_cast<byte>(
			(slot_rec[-REC_OLD_N_OWNED] & ~REC_N_OWNED_MASK)
			| (cur_n_owned - 1) << REC_N_OWNED_SHIFT);
	}

	page_mem_free(*block, current_rec, data_size, extra_size);

	/* Now we have decremented the number of owned records of the slot.
	If the number drops below PAGE_DIR_SLOT_MIN_N_OWNED, we balance the
	slots. */

	if (cur_n_owned <= PAGE_DIR_SLOT_MIN_N_OWNED) {
		page_dir_balance_slot(*block, cur_slot_no);
	}

	ut_ad(page_is_comp(block->page.frame)
	      ? page_simple_validate_new(block->page.frame)
	      : page_simple_validate_old(block->page.frame));
}

/** Apply a INSERT_HEAP_REDUNDANT or INSERT_REUSE_REDUNDANT record that was
written by page_cur_insert_rec_low() for a ROW_FORMAT=REDUNDANT page.
@param block      B-tree or R-tree page in ROW_FORMAT=COMPACT or DYNAMIC
@param reuse      false=allocate from PAGE_HEAP_TOP; true=reuse PAGE_FREE
@param prev       byte offset of the predecessor, relative to PAGE_OLD_INFIMUM
@param enc_hdr    encoded fixed-size header bits
@param hdr_c      number of common record header bytes with prev
@param data_c     number of common data bytes with prev
@param data       literal header and data bytes
@param data_len   length of the literal data, in bytes
@return whether the operation failed (inconcistency was noticed) */
bool page_apply_insert_redundant(const buf_block_t &block, bool reuse,
                                 ulint prev, ulint enc_hdr,
                                 size_t hdr_c, size_t data_c,
                                 const void *data, size_t data_len)
{
  page_t * const page= block.page.frame;
  const uint16_t n_slots= page_dir_get_n_slots(page);
  byte *page_n_heap= my_assume_aligned<2>(PAGE_N_HEAP + PAGE_HEADER + page);
  const uint16_t h= mach_read_from_2(page_n_heap);
  const page_id_t id(block.page.id());
  if (UNIV_UNLIKELY(n_slots < 2 || h < n_slots || h < PAGE_HEAP_NO_USER_LOW ||
                    h >= srv_page_size / REC_N_OLD_EXTRA_BYTES ||
                    !fil_page_index_page_check(page) ||
                    page_get_page_no(page) != id.page_no() ||
                    mach_read_from_2(my_assume_aligned<2>
                                     (PAGE_OLD_SUPREMUM - REC_NEXT + page))))
  {
corrupted:
    ib::error() << (reuse
                    ? "Not applying INSERT_REUSE_REDUNDANT"
                    " due to corruption on "
                    : "Not applying INSERT_HEAP_REDUNDANT"
                    " due to corruption on ")
                << id;
    return true;
  }

  byte * const last_slot= page_dir_get_nth_slot(page, n_slots - 1);
  byte * const page_heap_top= my_assume_aligned<2>
    (PAGE_HEAP_TOP + PAGE_HEADER + page);
  const byte *const heap_bot= &page[PAGE_OLD_SUPREMUM_END];
  byte *heap_top= page + mach_read_from_2(page_heap_top);
  if (UNIV_UNLIKELY(heap_bot > heap_top || heap_top > last_slot))
    goto corrupted;
  if (UNIV_UNLIKELY(mach_read_from_2(last_slot) != PAGE_OLD_SUPREMUM))
    goto corrupted;
  if (UNIV_UNLIKELY(mach_read_from_2(page_dir_get_nth_slot(page, 0)) !=
                                     PAGE_OLD_INFIMUM))
    goto corrupted;
  rec_t * const prev_rec= page + PAGE_OLD_INFIMUM + prev;
  if (!prev);
  else if (UNIV_UNLIKELY(heap_bot + (REC_N_OLD_EXTRA_BYTES + 1) > prev_rec ||
                         prev_rec > heap_top))
    goto corrupted;
  const ulint pn_fields= rec_get_bit_field_2(prev_rec, REC_OLD_N_FIELDS,
                                             REC_OLD_N_FIELDS_MASK,
                                             REC_OLD_N_FIELDS_SHIFT);
  if (UNIV_UNLIKELY(pn_fields == 0 || pn_fields > REC_MAX_N_FIELDS))
    goto corrupted;
  const ulint pextra_size= REC_N_OLD_EXTRA_BYTES +
    (rec_get_1byte_offs_flag(prev_rec) ? pn_fields : pn_fields * 2);
  if (prev_rec == &page[PAGE_OLD_INFIMUM]);
  else if (UNIV_UNLIKELY(prev_rec - pextra_size < heap_bot))
    goto corrupted;
  if (UNIV_UNLIKELY(hdr_c && prev_rec - hdr_c < heap_bot))
    goto corrupted;
  const ulint pdata_size= rec_get_data_size_old(prev_rec);
  if (UNIV_UNLIKELY(prev_rec + pdata_size > heap_top))
    goto corrupted;
  rec_t * const next_rec= page + mach_read_from_2(prev_rec - REC_NEXT);
  if (next_rec == page + PAGE_OLD_SUPREMUM);
  else if (UNIV_UNLIKELY(heap_bot + REC_N_OLD_EXTRA_BYTES > next_rec ||
                         next_rec > heap_top))
    goto corrupted;
  const bool is_short= (enc_hdr >> 2) & 1;
  const ulint n_fields= (enc_hdr >> 3) + 1;
  if (UNIV_UNLIKELY(n_fields > REC_MAX_N_FIELDS))
    goto corrupted;
  const ulint extra_size= REC_N_OLD_EXTRA_BYTES +
    (is_short ? n_fields : n_fields * 2);
  hdr_c+= REC_N_OLD_EXTRA_BYTES;
  if (UNIV_UNLIKELY(hdr_c > extra_size))
    goto corrupted;
  if (UNIV_UNLIKELY(extra_size - hdr_c > data_len))
    goto corrupted;
  /* We buffer all changes to the record header locally, so that
  we will avoid modifying the page before all consistency checks
  have been fulfilled. */
  alignas(2) byte insert_buf[REC_N_OLD_EXTRA_BYTES + REC_MAX_N_FIELDS * 2];

  ulint n_owned;
  rec_t *owner_rec= next_rec;
  for (ulint ns= PAGE_DIR_SLOT_MAX_N_OWNED;
       !(n_owned= rec_get_n_owned_old(owner_rec)); )
  {
    owner_rec= page + mach_read_from_2(owner_rec - REC_NEXT);
    if (owner_rec == &page[PAGE_OLD_SUPREMUM]);
    else if (UNIV_UNLIKELY(heap_bot + REC_N_OLD_EXTRA_BYTES > owner_rec ||
                           owner_rec > heap_top))
      goto corrupted;
    if (!ns--)
      goto corrupted; /* Corrupted (cyclic?) next-record list */
  }

  page_dir_slot_t *owner_slot= last_slot;

  if (n_owned > PAGE_DIR_SLOT_MAX_N_OWNED)
    goto corrupted;
  else
  {
    mach_write_to_2(insert_buf, owner_rec - page);
    static_assert(PAGE_DIR_SLOT_SIZE == 2, "compatibility");
    const page_dir_slot_t * const first_slot=
      page_dir_get_nth_slot(page, 0);

    while (memcmp_aligned<2>(owner_slot, insert_buf, 2))
      if ((owner_slot+= 2) == first_slot)
        goto corrupted;
  }

  memcpy(insert_buf, data, extra_size - hdr_c);
  byte *insert_rec= &insert_buf[extra_size];
  memcpy(insert_rec - hdr_c, prev_rec - hdr_c, hdr_c);
  rec_set_bit_field_1(insert_rec, (enc_hdr & 3) << 4,
                      REC_OLD_INFO_BITS, REC_INFO_BITS_MASK,
                      REC_INFO_BITS_SHIFT);
  rec_set_1byte_offs_flag(insert_rec, is_short);
  rec_set_n_fields_old(insert_rec, n_fields);
  rec_set_bit_field_1(insert_rec, 0, REC_OLD_N_OWNED,
                      REC_N_OWNED_MASK, REC_N_OWNED_SHIFT);

  const ulint data_size= rec_get_data_size_old(insert_rec);
  if (UNIV_UNLIKELY(data_c > data_size))
    goto corrupted;
  if (UNIV_UNLIKELY(extra_size - hdr_c + data_size - data_c != data_len))
    goto corrupted;

  /* Perform final consistency checks and then apply the change to the page. */
  byte *buf;
  if (reuse)
  {
    byte *page_free= my_assume_aligned<2>(PAGE_FREE + PAGE_HEADER +
                                          page);
    rec_t *free_rec= page + mach_read_from_2(page_free);
    if (UNIV_UNLIKELY(heap_bot + REC_N_OLD_EXTRA_BYTES > free_rec ||
                      free_rec > heap_top))
      goto corrupted;
    const ulint fn_fields= rec_get_n_fields_old(free_rec);
    const ulint fextra_size= REC_N_OLD_EXTRA_BYTES +
      (rec_get_1byte_offs_flag(free_rec) ? fn_fields : fn_fields * 2);
    if (UNIV_UNLIKELY(free_rec - fextra_size < heap_bot))
      goto corrupted;
    const ulint fdata_size= rec_get_data_size_old(free_rec);
    if (UNIV_UNLIKELY(free_rec + fdata_size > heap_top))
      goto corrupted;
    if (UNIV_UNLIKELY(extra_size + data_size > fextra_size + fdata_size))
      goto corrupted;
    byte *page_garbage= my_assume_aligned<2>(page_free + 2);
    if (UNIV_UNLIKELY(mach_read_from_2(page_garbage) <
                      fextra_size + fdata_size))
      goto corrupted;
    buf= free_rec - fextra_size;
    const rec_t *const next_free= page +
      mach_read_from_2(free_rec - REC_NEXT);
    if (next_free == page);
    else if (UNIV_UNLIKELY(next_free < &heap_bot[REC_N_OLD_EXTRA_BYTES + 1] ||
                           heap_top < next_free))
      goto corrupted;
    mach_write_to_2(page_garbage, mach_read_from_2(page_garbage) -
                    extra_size - data_size);
    rec_set_bit_field_2(insert_rec, rec_get_heap_no_old(free_rec),
                        REC_OLD_HEAP_NO, REC_HEAP_NO_MASK, REC_HEAP_NO_SHIFT);
    memcpy(page_free, free_rec - REC_NEXT, 2);
  }
  else
  {
    if (UNIV_UNLIKELY(heap_top + extra_size + data_size > last_slot))
      goto corrupted;
    rec_set_bit_field_2(insert_rec, h,
                        REC_OLD_HEAP_NO, REC_HEAP_NO_MASK, REC_HEAP_NO_SHIFT);
    mach_write_to_2(page_n_heap, h + 1);
    mach_write_to_2(page_heap_top,
                    mach_read_from_2(page_heap_top) + extra_size + data_size);
    buf= heap_top;
  }

  ut_ad(data_size - data_c == data_len - (extra_size - hdr_c));
  byte *page_last_insert= my_assume_aligned<2>(PAGE_LAST_INSERT + PAGE_HEADER +
                                               page);
  const uint16_t last_insert= mach_read_from_2(page_last_insert);
  memcpy(buf, insert_buf, extra_size);
  buf+= extra_size;
  mach_write_to_2(page_last_insert, buf - page);
  memcpy(prev_rec - REC_NEXT, page_last_insert, 2);
  memcpy(buf, prev_rec, data_c);
  memcpy(buf + data_c, static_cast<const byte*>(data) + (extra_size - hdr_c),
         data_len - (extra_size - hdr_c));
  rec_set_bit_field_1(owner_rec, n_owned + 1, REC_OLD_N_OWNED,
                      REC_N_OWNED_MASK, REC_N_OWNED_SHIFT);

  /* Update PAGE_DIRECTION_B, PAGE_N_DIRECTION if needed */
  if (page[FIL_PAGE_TYPE + 1] != byte(FIL_PAGE_RTREE))
  {
    byte *dir= &page[PAGE_DIRECTION_B + PAGE_HEADER];
    byte *n_dir= my_assume_aligned<2>
      (&page[PAGE_N_DIRECTION + PAGE_HEADER]);
    if (UNIV_UNLIKELY(!last_insert))
    {
no_direction:
      *dir= static_cast<byte>((*dir & ~((1U << 3) - 1)) | PAGE_NO_DIRECTION);
      memset(n_dir, 0, 2);
    }
    else if (page + last_insert == prev_rec &&
             (*dir & ((1U << 3) - 1)) != PAGE_LEFT)
    {
      *dir= static_cast<byte>((*dir & ~((1U << 3) - 1)) | PAGE_RIGHT);
inc_dir:
      mach_write_to_2(n_dir, mach_read_from_2(n_dir) + 1);
    }
    else if (next_rec == page + last_insert &&
             (*dir & ((1U << 3) - 1)) != PAGE_RIGHT)
    {
      *dir= static_cast<byte>((*dir & ~((1U << 3) - 1)) | PAGE_LEFT);
      goto inc_dir;
    }
    else
      goto no_direction;
  }

  /* Update PAGE_N_RECS. */
  byte *page_n_recs= my_assume_aligned<2>(PAGE_N_RECS + PAGE_HEADER + page);

  mach_write_to_2(page_n_recs, mach_read_from_2(page_n_recs) + 1);

  if (UNIV_UNLIKELY(n_owned == PAGE_DIR_SLOT_MAX_N_OWNED))
    return page_dir_split_slot(block, owner_slot);
  ut_ad(page_simple_validate_old(page));
  return false;
}

/** Apply a INSERT_HEAP_DYNAMIC or INSERT_REUSE_DYNAMIC record that was
written by page_cur_insert_rec_low() for a ROW_FORMAT=COMPACT or DYNAMIC page.
@param block      B-tree or R-tree page in ROW_FORMAT=COMPACT or DYNAMIC
@param reuse      false=allocate from PAGE_HEAP_TOP; true=reuse PAGE_FREE
@param prev       byte offset of the predecessor, relative to PAGE_NEW_INFIMUM
@param shift      unless !reuse: number of bytes the PAGE_FREE is moving
@param enc_hdr_l  number of copied record header bytes, plus record type bits
@param hdr_c      number of common record header bytes with prev
@param data_c     number of common data bytes with prev
@param data       literal header and data bytes
@param data_len   length of the literal data, in bytes
@return whether the operation failed (inconcistency was noticed) */
bool page_apply_insert_dynamic(const buf_block_t &block, bool reuse,
                               ulint prev, ulint shift, ulint enc_hdr_l,
                               size_t hdr_c, size_t data_c,
                               const void *data, size_t data_len)
{
  page_t * const page= block.page.frame;
  const uint16_t n_slots= page_dir_get_n_slots(page);
  byte *page_n_heap= my_assume_aligned<2>(PAGE_N_HEAP + PAGE_HEADER + page);
  ulint h= mach_read_from_2(page_n_heap);
  const page_id_t id(block.page.id());
  if (UNIV_UNLIKELY(n_slots < 2 || h < (PAGE_HEAP_NO_USER_LOW | 0x8000) ||
                    (h & 0x7fff) >= srv_page_size / REC_N_NEW_EXTRA_BYTES ||
                    (h & 0x7fff) < n_slots ||
                    !fil_page_index_page_check(page) ||
                    page_get_page_no(page) != id.page_no() ||
                    mach_read_from_2(my_assume_aligned<2>
                                     (PAGE_NEW_SUPREMUM - REC_NEXT + page)) ||
                    ((enc_hdr_l & REC_STATUS_INSTANT) &&
                     !page_is_leaf(page)) ||
                    (enc_hdr_l >> 3) > data_len))
  {
corrupted:
    ib::error() << (reuse
                    ? "Not applying INSERT_REUSE_DYNAMIC"
                    " due to corruption on "
                    : "Not applying INSERT_HEAP_DYNAMIC"
                    " due to corruption on ")
                << id;
    return true;
  }

  byte * const last_slot= page_dir_get_nth_slot(page, n_slots - 1);
  byte * const page_heap_top= my_assume_aligned<2>
    (PAGE_HEAP_TOP + PAGE_HEADER + page);
  const byte *const heap_bot= &page[PAGE_NEW_SUPREMUM_END];
  byte *heap_top= page + mach_read_from_2(page_heap_top);
  if (UNIV_UNLIKELY(heap_bot > heap_top || heap_top > last_slot))
    goto corrupted;
  if (UNIV_UNLIKELY(mach_read_from_2(last_slot) != PAGE_NEW_SUPREMUM))
    goto corrupted;
  if (UNIV_UNLIKELY(mach_read_from_2(page_dir_get_nth_slot(page, 0)) !=
                                     PAGE_NEW_INFIMUM))
    goto corrupted;

  uint16_t n= static_cast<uint16_t>(PAGE_NEW_INFIMUM + prev);
  rec_t *prev_rec= page + n;
  n= static_cast<uint16_t>(n + mach_read_from_2(prev_rec - REC_NEXT));
  if (!prev);
  else if (UNIV_UNLIKELY(heap_bot + REC_N_NEW_EXTRA_BYTES > prev_rec ||
                         prev_rec > heap_top))
    goto corrupted;

  rec_t * const next_rec= page + n;
  if (next_rec == page + PAGE_NEW_SUPREMUM);
  else if (UNIV_UNLIKELY(heap_bot + REC_N_NEW_EXTRA_BYTES > next_rec ||
                         next_rec > heap_top))
    goto corrupted;

  ulint n_owned;
  rec_t *owner_rec= next_rec;
  n= static_cast<uint16_t>(next_rec - page);

  for (ulint ns= PAGE_DIR_SLOT_MAX_N_OWNED;
       !(n_owned= rec_get_n_owned_new(owner_rec)); )
  {
    n= static_cast<uint16_t>(n + mach_read_from_2(owner_rec - REC_NEXT));
    owner_rec= page + n;
    if (n == PAGE_NEW_SUPREMUM);
    else if (UNIV_UNLIKELY(heap_bot + REC_N_NEW_EXTRA_BYTES > owner_rec ||
                           owner_rec > heap_top))
      goto corrupted;
    if (!ns--)
      goto corrupted; /* Corrupted (cyclic?) next-record list */
  }

  page_dir_slot_t* owner_slot= last_slot;

  if (n_owned > PAGE_DIR_SLOT_MAX_N_OWNED)
    goto corrupted;
  else
  {
    static_assert(PAGE_DIR_SLOT_SIZE == 2, "compatibility");
    alignas(2) byte slot_buf[2];
    mach_write_to_2(slot_buf, owner_rec - page);
    const page_dir_slot_t * const first_slot=
      page_dir_get_nth_slot(page, 0);

    while (memcmp_aligned<2>(owner_slot, slot_buf, 2))
      if ((owner_slot+= 2) == first_slot)
        goto corrupted;
  }

  const ulint extra_size= REC_N_NEW_EXTRA_BYTES + hdr_c + (enc_hdr_l >> 3);
  const ulint data_size= data_c + data_len - (enc_hdr_l >> 3);

  /* Perform final consistency checks and then apply the change to the page. */
  byte *buf;
  if (reuse)
  {
    byte *page_free= my_assume_aligned<2>(PAGE_FREE + PAGE_HEADER + page);
    rec_t *free_rec= page + mach_read_from_2(page_free);
    if (UNIV_UNLIKELY(heap_bot + REC_N_NEW_EXTRA_BYTES > free_rec ||
                      free_rec > heap_top))
      goto corrupted;
    buf= free_rec - extra_size;
    if (shift & 1)
      buf-= shift >> 1;
    else
      buf+= shift >> 1;

    if (UNIV_UNLIKELY(heap_bot > buf ||
                      &buf[extra_size + data_size] > heap_top))
      goto corrupted;
    byte *page_garbage= my_assume_aligned<2>(page_free + 2);
    if (UNIV_UNLIKELY(mach_read_from_2(page_garbage) < extra_size + data_size))
      goto corrupted;
    if ((n= mach_read_from_2(free_rec - REC_NEXT)) != 0)
    {
      n= static_cast<uint16_t>(n + free_rec - page);
      if (UNIV_UNLIKELY(n < PAGE_NEW_SUPREMUM_END + REC_N_NEW_EXTRA_BYTES ||
                        heap_top < page + n))
        goto corrupted;
    }
    mach_write_to_2(page_free, n);
    mach_write_to_2(page_garbage, mach_read_from_2(page_garbage) -
                    (extra_size + data_size));
    h= rec_get_heap_no_new(free_rec);
  }
  else
  {
    if (UNIV_UNLIKELY(heap_top + extra_size + data_size > last_slot))
      goto corrupted;
    mach_write_to_2(page_n_heap, h + 1);
    h&= 0x7fff;
    mach_write_to_2(page_heap_top,
                    mach_read_from_2(page_heap_top) + extra_size + data_size);
    buf= heap_top;
  }

  memcpy(buf, data, (enc_hdr_l >> 3));
  buf+= enc_hdr_l >> 3;
  data_len-= enc_hdr_l >> 3;
  data= &static_cast<const byte*>(data)[enc_hdr_l >> 3];

  memcpy(buf, prev_rec - REC_N_NEW_EXTRA_BYTES - hdr_c, hdr_c);
  buf+= hdr_c;
  *buf++= static_cast<byte>((enc_hdr_l & 3) << 4); /* info_bits; n_owned=0 */
  *buf++= static_cast<byte>(h >> 5); /* MSB of heap number */
  h= (h & ((1U << 5) - 1)) << 3;
  static_assert(REC_STATUS_ORDINARY == 0, "compatibility");
  static_assert(REC_STATUS_INSTANT == 4, "compatibility");
  if (page_is_leaf(page))
    h|= enc_hdr_l & REC_STATUS_INSTANT;
  else
  {
    ut_ad(!(enc_hdr_l & REC_STATUS_INSTANT)); /* Checked at the start */
    h|= REC_STATUS_NODE_PTR;
  }
  *buf++= static_cast<byte>(h); /* LSB of heap number, and status */
  static_assert(REC_NEXT == 2, "compatibility");
  buf+= REC_NEXT;
  mach_write_to_2(buf - REC_NEXT, static_cast<uint16_t>(next_rec - buf));
  byte *page_last_insert= my_assume_aligned<2>(PAGE_LAST_INSERT + PAGE_HEADER +
                                               page);
  const uint16_t last_insert= mach_read_from_2(page_last_insert);
  mach_write_to_2(page_last_insert, buf - page);
  mach_write_to_2(prev_rec - REC_NEXT, static_cast<uint16_t>(buf - prev_rec));
  memcpy(buf, prev_rec, data_c);
  buf+= data_c;
  memcpy(buf, data, data_len);

  rec_set_bit_field_1(owner_rec, n_owned + 1, REC_NEW_N_OWNED,
                      REC_N_OWNED_MASK, REC_N_OWNED_SHIFT);

  /* Update PAGE_DIRECTION_B, PAGE_N_DIRECTION if needed */
  if (page[FIL_PAGE_TYPE + 1] != byte(FIL_PAGE_RTREE))
  {
    byte *dir= &page[PAGE_DIRECTION_B + PAGE_HEADER];
    byte *n_dir= my_assume_aligned<2>(&page[PAGE_N_DIRECTION + PAGE_HEADER]);
    if (UNIV_UNLIKELY(!last_insert))
    {
no_direction:
      *dir= static_cast<byte>((*dir & ~((1U << 3) - 1)) | PAGE_NO_DIRECTION);
      memset(n_dir, 0, 2);
    }
    else if (page + last_insert == prev_rec &&
             (*dir & ((1U << 3) - 1)) != PAGE_LEFT)
    {
      *dir= static_cast<byte>((*dir & ~((1U << 3) - 1)) | PAGE_RIGHT);
inc_dir:
      mach_write_to_2(n_dir, mach_read_from_2(n_dir) + 1);
    }
    else if (next_rec == page + last_insert &&
             (*dir & ((1U << 3) - 1)) != PAGE_RIGHT)
    {
      *dir= static_cast<byte>((*dir & ~((1U << 3) - 1)) | PAGE_LEFT);
      goto inc_dir;
    }
    else
      goto no_direction;
  }

  /* Update PAGE_N_RECS. */
  byte *page_n_recs= my_assume_aligned<2>(PAGE_N_RECS + PAGE_HEADER + page);

  mach_write_to_2(page_n_recs, mach_read_from_2(page_n_recs) + 1);

  if (UNIV_UNLIKELY(n_owned == PAGE_DIR_SLOT_MAX_N_OWNED))
    return page_dir_split_slot(block, owner_slot);
  ut_ad(page_simple_validate_new(page));
  return false;
}

/** Apply a DELETE_ROW_FORMAT_REDUNDANT record that was written by
page_cur_delete_rec() for a ROW_FORMAT=REDUNDANT page.
@param block    B-tree or R-tree page in ROW_FORMAT=REDUNDANT
@param prev     byte offset of the predecessor, relative to PAGE_OLD_INFIMUM
@return whether the operation failed (inconcistency was noticed) */
bool page_apply_delete_redundant(const buf_block_t &block, ulint prev)
{
  page_t * const page= block.page.frame;
  const uint16_t n_slots= page_dir_get_n_slots(page);
  ulint n_recs= page_get_n_recs(page);
  const page_id_t id(block.page.id());

  if (UNIV_UNLIKELY(!n_recs || n_slots < 2 ||
                    !fil_page_index_page_check(page) ||
                    page_get_page_no(page) != id.page_no() ||
                    mach_read_from_2(my_assume_aligned<2>
                                     (PAGE_OLD_SUPREMUM - REC_NEXT + page)) ||
                    page_is_comp(page)))
  {
corrupted:
    ib::error() << "Not applying DELETE_ROW_FORMAT_REDUNDANT"
                   " due to corruption on " << id;
    return true;
  }

  byte *slot= page_dir_get_nth_slot(page, n_slots - 1);
  rec_t *prev_rec= page + PAGE_OLD_INFIMUM + prev;
  if (UNIV_UNLIKELY(prev_rec > slot))
    goto corrupted;
  uint16_t n= mach_read_from_2(prev_rec - REC_NEXT);
  rec_t *rec= page + n;
  if (UNIV_UNLIKELY(n < PAGE_OLD_SUPREMUM_END + REC_N_OLD_EXTRA_BYTES ||
                    slot < rec))
    goto corrupted;
  const ulint extra_size= REC_N_OLD_EXTRA_BYTES + rec_get_n_fields_old(rec) *
    (rec_get_1byte_offs_flag(rec) ? 1 : 2);
  const ulint data_size= rec_get_data_size_old(rec);
  if (UNIV_UNLIKELY(n < PAGE_OLD_SUPREMUM_END + extra_size ||
                    slot < rec + data_size))
    goto corrupted;

  n= mach_read_from_2(rec - REC_NEXT);
  rec_t *next= page + n;
  if (n == PAGE_OLD_SUPREMUM);
  else if (UNIV_UNLIKELY(n < PAGE_OLD_SUPREMUM_END + REC_N_OLD_EXTRA_BYTES ||
                         slot < next))
    goto corrupted;

  rec_t *s= rec;
  ulint slot_owned;
  for (ulint i= n_recs; !(slot_owned= rec_get_n_owned_old(s)); )
  {
    n= mach_read_from_2(s - REC_NEXT);
    s= page + n;
    if (n == PAGE_OLD_SUPREMUM);
    else if (UNIV_UNLIKELY(n < PAGE_OLD_SUPREMUM_END + REC_N_OLD_EXTRA_BYTES ||
                           slot < s))
      goto corrupted;
    if (UNIV_UNLIKELY(!i--)) /* Corrupted (cyclic?) next-record list */
      goto corrupted;
  }
  slot_owned--;

  /* The first slot is always pointing to the infimum record.
  Find the directory slot pointing to s. */
  const byte * const first_slot= page + srv_page_size - (PAGE_DIR + 2);
  alignas(2) byte slot_offs[2];
  mach_write_to_2(slot_offs, s - page);
  static_assert(PAGE_DIR_SLOT_SIZE == 2, "compatibility");

  while (memcmp_aligned<2>(slot, slot_offs, 2))
    if ((slot+= 2) == first_slot)
      goto corrupted;

  if (rec == s)
  {
    s= prev_rec;
    mach_write_to_2(slot, s - page);
  }

  memcpy(prev_rec - REC_NEXT, rec - REC_NEXT, 2);
  s-= REC_OLD_N_OWNED;
  *s= static_cast<byte>((*s & ~REC_N_OWNED_MASK) |
                        slot_owned << REC_N_OWNED_SHIFT);
  page_mem_free(block, rec, data_size, extra_size);

  if (slot_owned < PAGE_DIR_SLOT_MIN_N_OWNED)
    page_dir_balance_slot(block, (first_slot - slot) / 2);

  ut_ad(page_simple_validate_old(page));
  return false;
}

/** Apply a DELETE_ROW_FORMAT_DYNAMIC record that was written by
page_cur_delete_rec() for a ROW_FORMAT=COMPACT or DYNAMIC page.
@param block      B-tree or R-tree page in ROW_FORMAT=COMPACT or DYNAMIC
@param prev       byte offset of the predecessor, relative to PAGE_NEW_INFIMUM
@param hdr_size   record header size, excluding REC_N_NEW_EXTRA_BYTES
@param data_size  data payload size, in bytes
@return whether the operation failed (inconcistency was noticed) */
bool page_apply_delete_dynamic(const buf_block_t &block, ulint prev,
                               size_t hdr_size, size_t data_size)
{
  page_t * const page= block.page.frame;
  const uint16_t n_slots= page_dir_get_n_slots(page);
  ulint n_recs= page_get_n_recs(page);
  const page_id_t id(block.page.id());

  if (UNIV_UNLIKELY(!n_recs || n_slots < 2 ||
                    !fil_page_index_page_check(page) ||
                    page_get_page_no(page) != id.page_no() ||
                    mach_read_from_2(my_assume_aligned<2>
                                     (PAGE_NEW_SUPREMUM - REC_NEXT + page)) ||
                    !page_is_comp(page)))
  {
corrupted:
    ib::error() << "Not applying DELETE_ROW_FORMAT_DYNAMIC"
                   " due to corruption on " << id;
    return true;
  }

  byte *slot= page_dir_get_nth_slot(page, n_slots - 1);
  uint16_t n= static_cast<uint16_t>(PAGE_NEW_INFIMUM + prev);
  rec_t *prev_rec= page + n;
  if (UNIV_UNLIKELY(prev_rec > slot))
    goto corrupted;
  n= static_cast<uint16_t>(n + mach_read_from_2(prev_rec - REC_NEXT));
  rec_t *rec= page + n;
  if (UNIV_UNLIKELY(n < PAGE_NEW_SUPREMUM_END + REC_N_NEW_EXTRA_BYTES ||
                    slot < rec))
    goto corrupted;
  const ulint extra_size= REC_N_NEW_EXTRA_BYTES + hdr_size;
  if (UNIV_UNLIKELY(n < PAGE_NEW_SUPREMUM_END + extra_size ||
                    slot < rec + data_size))
    goto corrupted;
  n= static_cast<uint16_t>(n + mach_read_from_2(rec - REC_NEXT));
  rec_t *next= page + n;
  if (n == PAGE_NEW_SUPREMUM);
  else if (UNIV_UNLIKELY(n < PAGE_NEW_SUPREMUM_END + REC_N_NEW_EXTRA_BYTES ||
                         slot < next))
    goto corrupted;

  rec_t *s= rec;
  n= static_cast<uint16_t>(rec - page);
  ulint slot_owned;
  for (ulint i= n_recs; !(slot_owned= rec_get_n_owned_new(s)); )
  {
    const uint16_t next= mach_read_from_2(s - REC_NEXT);
    if (UNIV_UNLIKELY(next < REC_N_NEW_EXTRA_BYTES ||
                      next > static_cast<uint16_t>(-REC_N_NEW_EXTRA_BYTES)))
      goto corrupted;
    n= static_cast<uint16_t>(n + next);
    s= page + n;
    if (n == PAGE_NEW_SUPREMUM);
    else if (UNIV_UNLIKELY(n < PAGE_NEW_SUPREMUM_END + REC_N_NEW_EXTRA_BYTES ||
                           slot < s))
      goto corrupted;
    if (UNIV_UNLIKELY(!i--)) /* Corrupted (cyclic?) next-record list */
      goto corrupted;
  }
  slot_owned--;

  /* The first slot is always pointing to the infimum record.
  Find the directory slot pointing to s. */
  const byte * const first_slot= page + srv_page_size - (PAGE_DIR + 2);
  alignas(2) byte slot_offs[2];
  mach_write_to_2(slot_offs, s - page);
  static_assert(PAGE_DIR_SLOT_SIZE == 2, "compatibility");

  while (memcmp_aligned<2>(slot, slot_offs, 2))
    if ((slot+= 2) == first_slot)
      goto corrupted;

  if (rec == s)
  {
    s= prev_rec;
    mach_write_to_2(slot, s - page);
  }

  mach_write_to_2(prev_rec - REC_NEXT, static_cast<uint16_t>(next - prev_rec));
  s-= REC_NEW_N_OWNED;
  *s= static_cast<byte>((*s & ~REC_N_OWNED_MASK) |
                        slot_owned << REC_N_OWNED_SHIFT);
  page_mem_free(block, rec, data_size, extra_size);

  if (slot_owned < PAGE_DIR_SLOT_MIN_N_OWNED)
    page_dir_balance_slot(block, (first_slot - slot) / 2);

  ut_ad(page_simple_validate_new(page));
  return false;
}

#ifdef UNIV_COMPILE_TEST_FUNCS

/*******************************************************************//**
Print the first n numbers, generated by ut_rnd_gen() to make sure
(visually) that it works properly. */
void
test_ut_rnd_gen(
	int	n)	/*!< in: print first n numbers */
{
	int			i;
	unsigned long long	rnd;

	for (i = 0; i < n; i++) {
		rnd = ut_rnd_gen();
		printf("%llu\t%%2=%llu %%3=%llu %%5=%llu %%7=%llu %%11=%llu\n",
		       rnd,
		       rnd % 2,
		       rnd % 3,
		       rnd % 5,
		       rnd % 7,
		       rnd % 11);
	}
}

#endif /* UNIV_COMPILE_TEST_FUNCS */
