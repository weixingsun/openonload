/*
** Copyright 2005-2012  Solarflare Communications Inc.
**                      7505 Irvine Center Drive, Irvine, CA 92618, USA
** Copyright 2002-2005  Level 5 Networks Inc.
**
** This program is free software; you can redistribute it and/or modify it
** under the terms of version 2 of the GNU General Public License as
** published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/


#include <ci/driver/efab/debug.h>
#include <ci/driver/efab/efch.h>
#include <ci/efrm/driver_private.h> /* FIXME for efrm_rm_table */
#include <ci/efrm/nic_table.h>
#include <ci/efch/op_types.h>
#include <ci/driver/efab/linux_char_internal.h>
#include "char_internal.h"
#include "efch_intf_ver.h"


/* Versions of the user-kernel interface that this driver is compatible
 * with.  Each time we settle on a stable interface, the version string is
 * entered into this table.  The index in this table is the
 * interface-version-ID (intf_ver_id), which is used to identify the
 * particular interface version.
 */
static const char* supported_intf_vers[] = {
  /* openonload-201109-u2, enterpriseonload-2.0.x.y */
  "b7722b6c157e5218c23d18a1adf77b66",
  /* This version -- autogenerated from header. */
  EFCH_INTF_VER,
};


efch_resource_ops *efch_ops_table[] = {
  &efch_iobufset_ops,
  &efch_vi_ops,
  &efch_vi_set_ops,
  NULL,
  &efch_memreg_ops,
  &efch_pd_ops,
};


#ifndef NDEBUG
static void
rs_dump_inner(ci_resource_table_t *rt, efch_resource_t *rs)
{
  if (!rs) {
    ci_log ("  rs %p", rs);
    return;
  }
  if (rs->rs_base != NULL)
    ci_log("    rs %p " EFRM_RESOURCE_FMT " rt %p ops %p",
           rs, EFRM_RESOURCE_PRI_ARG(rs->rs_base), rt, rs->rs_ops);
  else
    ci_log("    rs %p rt %p ops %p",
           rs, rt, rs->rs_ops);
  if (rs->rs_ops && rs->rs_ops->rm_dump)
    rs->rs_ops->rm_dump(rs->rs_base, rt, "      ");
}

static void priv_dump_inner(ci_resource_table_t* rt, const char* context)
{
  unsigned i;
  ci_log("  rt %p context [%s] table %p highwater %d size %d ",
	 rt, context, rt->resource_table,
	 rt->resource_table_highwater, rt->resource_table_size);
  for( i = 0; i < rt->resource_table_highwater; ++i ) {
    efch_resource_t *rs = rt->resource_table[i];
    if (rs != NULL && rs->rs_base != NULL) {
      unsigned type = rs->rs_base->rs_type;
      ci_log("  id=%d: type=%d addr=%p handle=" EFRM_RESOURCE_FMT, i, type,
	     rs, EFRM_RESOURCE_PRI_ARG(rs->rs_base));
      rs_dump_inner(rt, rs);
    }
  }
}


/*! Dump a specific priv, or if priv==NULL and CI_CFG_PRIVATE_T_DEBUG_LIST
 * is defined.
 */
static void
efch_priv_dump(ci_resource_table_t *rt, const char *context)
{
  if (rt == NULL) {
#if CI_CFG_PRIVATE_T_DEBUG_LIST
    ci_lock_lock(&priv_list_lock);
    list_for_each_entry(rt, &priv_list, priv_list) {
      ci_log("rt %d: %p", i, rt);
      if (rt == NULL) break;
      priv_dump_inner(rt, context);
      i++;
    }
    ci_lock_unlock(&priv_list_lock);
#else
    ci_log("Note: CI_CFG_PRIVATE_T_DEBUG_LIST is false and efch_priv_dump on "
	   "NULL rt called");
#endif
  } else {
    priv_dump_inner(rt, context);
  }
}

# define EFAB_PRIV_DUMP(priv) efch_priv_dump((priv), __FUNCTION__)
#else
# define EFAB_PRIV_DUMP(_x)   do{}while(0)
#endif

void
efch_resource_dump(efch_resource_t* rs)
{
  ci_assert(rs);

  if (rs->rs_base != NULL)
    ci_log("efch_resource_dump: %d "EFRM_RESOURCE_FMT" i=%d",
           rs->rs_base->rs_type, EFRM_RESOURCE_PRI_ARG(rs->rs_base),
           rs->rs_base->rs_instance);
  if( rs->rs_ops->rm_dump )
    rs->rs_ops->rm_dump(rs->rs_base, NULL, "");
}


int efch_lookup_rs(int fd, efch_resource_id_t rs_id, int rs_type,
                   struct efrm_resource **rs_out)
{
  ci_private_char_t *priv;
  efch_resource_t *rs;
  struct file *file;
  int rc;

  if ((file = fget(fd)) == NULL)
    return -EBADF;
  if (file->f_op == &ci_char_fops) {
    priv = file->private_data;
    rc = efch_resource_id_lookup(rs_id, &priv->rt, &rs);
    if (rc == 0) {
      if (rs->rs_base->rs_type == rs_type) {
        efrm_resource_ref(rs->rs_base);
        *rs_out = rs->rs_base;
      }
      else
        rc = -EINVAL;
    }
  }
  else
    rc = -EINVAL;
  fput(file);
  return rc;
}
EXPORT_SYMBOL(efch_lookup_rs);


static int
grow_priv_table(ci_resource_table_t *rt)
{
  efch_resource_t** table;

  /* We are only permitted to grow the table once. */
  if( rt->resource_table_size == EFRM_RESOURCE_MAX_PER_FD )  {
    DEBUGERR( ci_log("%s: reached maxium number of resources %d", 
                     __FUNCTION__, rt->resource_table_size) );
    return -ENOSPC;
  }

  table = kmalloc(sizeof(struct efrm_resource *) * EFRM_RESOURCE_MAX_PER_FD,
                  GFP_KERNEL);
  if( ! table )  return -ENOMEM;

  /* Copy the existing table and zero-out the rest. */
  memcpy(table, rt->resource_table_static,
         sizeof(struct efrm_resource *) * CI_DEFAULT_RESOURCE_TABLE_SIZE);
  memset(table + CI_DEFAULT_RESOURCE_TABLE_SIZE, 0,
         (EFRM_RESOURCE_MAX_PER_FD - CI_DEFAULT_RESOURCE_TABLE_SIZE)
         * sizeof(table[0]));

  /* Yuk - the cast below to void* is horrid, but necessary to avoid a
   * type-punned ptr break strict aliasing" warning.  We know this cast is safe,
   * because it's to a CAS operation that is an "asm volatile"
   */
  if( ci_cas_uintptr_succeed((void*) &rt->resource_table,
                             (ci_uintptr_t) rt->resource_table_static,
                             (ci_uintptr_t) table) ) {
    /* Tricky: Someone could have concurrently written an entry into
    ** [resource_table_static], so we need to copy those entries out again
    ** just in case.  The "badness" is that an entry can temporarily
    ** disappear from the table.  However, it is safe wrt ref counting the
    ** resources, and only happens if people are inserting stuff
    ** concurrently.  (We have no code that does that, but we still need to
    ** protect against it for security).
    */
    memcpy(table, rt->resource_table_static,
           sizeof(struct efrm_resource *) * CI_DEFAULT_RESOURCE_TABLE_SIZE);
    rt->resource_table_size = EFRM_RESOURCE_MAX_PER_FD;
    return 0;
  }

  /* failed to swap; someone else must have got there first */
  ci_free(table);
  return 0;
}


static int
efch_resource_manager_allocate_id(ci_resource_table_t* rt,
                                  efch_resource_t* rs,
                                  efch_resource_id_t* id_out)
{
  /* Please talk to djr if you feel tempted to change this function at all.
  ** The use of the temporary [table] is particularly subtle, and is
  ** carefully placed to avoid the need for additional memory barriers..
  */

  efch_resource_t** table;
  ci_uint32 index;
  int rc;

  ci_assert(rt);
  ci_assert(id_out);

  DEBUGRES(EFAB_PRIV_DUMP(rt));

  /* Attempt to grab a valid index. */
  do {
  loop_again:
    index = rt->resource_table_highwater;
    table = rt->resource_table;
    /* We must not allow [resource_table_highwater] to exceed the size of
    ** the table, even temporarily. */
    if( index >= rt->resource_table_size ) {
      if( (rc = grow_priv_table(rt)) < 0 )  return rc;
      goto loop_again;
    }
  }
  while( ci_cas32u_fail(&rt->resource_table_highwater, index, index + 1) );

  ci_assert_lt(index,rt->resource_table_size);
  ci_assert_equal(table[index],NULL);

  table[index] = rs;
  id_out->index = index;

  if( table != rt->resource_table ) {
    /* Oh dear: The table got resized under our feet.  Better put this
    ** entry into the new table too.  See grow_priv_table() for other racey
    ** cases. */
    ci_assert_equal(rt->resource_table[index],NULL);
    rt->resource_table[index] = rs;
  }

  return 0;
}


static int find_intf_ver(const char* ver)
{
  int n_vers = sizeof(supported_intf_vers) / sizeof(supported_intf_vers[0]);
  int i;
  for( i = 0; i < n_vers; ++i )
    if( strcmp(ver, supported_intf_vers[i]) == 0 )
      return i;
  return -1;
}


int
efch_resource_alloc(ci_resource_table_t* rt, ci_resource_alloc_t* alloc)
{
  efch_resource_t* rs;
  efch_resource_ops* ops;
  char intf_ver[EFCH_INTF_VER_LEN + 1];
  int rc, intf_ver_id;

  ci_assert(alloc);
  ci_assert(rt);

  ci_assert(sizeof(intf_ver) >= sizeof(alloc->intf_ver));
  memcpy(intf_ver, alloc->intf_ver, sizeof(alloc->intf_ver));
  intf_ver[EFCH_INTF_VER_LEN] = '\0';
  if( (intf_ver_id = find_intf_ver(intf_ver)) < 0 ) {
    ci_log("%s: ERROR: incompatible interface version", __FUNCTION__);
    ci_log("%s: user_ver=%s driver_ver=%s",
           __FUNCTION__, intf_ver, EFCH_INTF_VER);
    return -ELIBACC;
  }

  if( alloc->ra_type >= EFRM_RESOURCE_NUM) {
    ci_log("%s: unsupported type %d", __FUNCTION__, alloc->ra_type);
    return -EOPNOTSUPP;
  }

  if ((ops = efch_ops_table[alloc->ra_type]) == NULL) {
    ci_log("%s[%d] resource manager not installed", __FUNCTION__,
           alloc->ra_type);
    return -ENOENT;
  }
  ci_assert(ops->rm_alloc);

  rs = ci_alloc(sizeof(*rs));
  if (rs == NULL)
    return -ENOMEM;

  rc = ops->rm_alloc(alloc, rt, rs, intf_ver_id);
  if( rc < 0 ) {
    DEBUGERR(ci_log("%s: did not allocate %d (%d)", __FUNCTION__,
	            alloc->ra_type, rc));
    ci_free(rs);
    return rc;
  }
  rs->rs_ops = ops;

  /* Insert it into the ci_resource_table_t resource table */
  rc = efch_resource_manager_allocate_id(rt, rs, &alloc->out_id);
  if (rc == 0) {
    DEBUGRES(ci_log("%s: allocated type=%x "EFCH_RESOURCE_ID_FMT,
		     __FUNCTION__, alloc->ra_type,
		     EFCH_RESOURCE_ID_PRI_ARG(alloc->out_id)));
    return 0;
  }

  efch_resource_free(rs);
  return rc;
}


void efch_resource_free(efch_resource_t *rs)
{
  if (rs->rs_ops->rm_free != NULL)
    rs->rs_ops->rm_free(rs);
  if (rs->rs_base != NULL)
    efrm_resource_release(rs->rs_base);
  CI_DEBUG_ZERO(rs);
  ci_free(rs);
}


int
efch_resource_op(ci_resource_table_t* rt,
                     ci_resource_op_t* op, int* copy_out
		     CI_BLOCKING_CTX_ARG(ci_blocking_ctx_t bc))
{
  efch_resource_t* rs;
  int rc;

  rc = efch_resource_id_lookup(op->id, rt, &rs);
  if( rc < 0 ) {
    DEBUGERR(ci_log("%s: hwm:%d op=%#x id="EFCH_RESOURCE_ID_FMT" rc=%d",
        __FUNCTION__, rt->resource_table_highwater, 
        op->op, EFCH_RESOURCE_ID_PRI_ARG(op->id), rc));
    goto done;
  }

  DEBUGVERB(ci_log("%s: id="EFCH_RESOURCE_ID_FMT" op=%x",
                   __FUNCTION__, EFCH_RESOURCE_ID_PRI_ARG(op->id), op->op));

  if (op->op == CI_RSOP_DUMP) {
    efch_resource_dump(rs);
    rc = 0;
  } else if (rs->rs_ops->rm_rsops == NULL) {
    rc = -EOPNOTSUPP;
  } else {
    rc = rs->rs_ops->rm_rsops(rs, rt, op, copy_out CI_BLOCKING_CTX_ARG(bc));
  }
done:
  return rc;
}