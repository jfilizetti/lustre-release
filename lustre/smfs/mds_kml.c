/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Lustre filesystem abstraction routines
 *
 *  Copyright (C) 2004 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define DEBUG_SUBSYSTEM S_SM

#include <linux/kmod.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/obd_class.h>
#include <linux/obd_support.h>
#include <linux/lustre_lib.h>
#include <linux/lustre_idl.h>
#include <linux/lustre_fsfilt.h>
#include <linux/lustre_lite.h>
#include <linux/lustre_mds.h>
#include <linux/lustre_smfs.h>
#include "smfs_internal.h"

static int mds_rec_link_pack(char *buffer, struct dentry *dentry,
                             struct inode *dir, void *data1, void *data2)
{
        struct mdc_op_data op_data;
        struct mds_kml_pack_info *mkpi;
        struct dentry *tgt = (struct dentry *)data1;
        struct dentry *src = (struct dentry *)data2;
        void   *tmp = NULL;
        struct lustre_msg *msg = NULL;
        int    rc = 0;

        ll_prepare_mdc_op_data(&op_data, src->d_inode, dir, tgt->d_name.name,
                               tgt->d_name.len, 0);

        PACK_KML_REC_INIT(buffer, MDS_REINT);

        mkpi = (struct mds_kml_pack_info*)buffer;

        mkpi->mpi_bufcount = 2;
        mkpi->mpi_size[0] = sizeof(struct mds_rec_link);
        mkpi->mpi_size[1] = op_data.namelen + 1;

        /*the mds reint log format
         *opcode + mkpi + request
         */
        msg = (struct lustre_msg *)(buffer + sizeof(*mkpi));
        lustre_init_msg(msg, mkpi->mpi_bufcount, mkpi->mpi_size, NULL);

        tmp = mdc_link_pack(msg, 0, &op_data);
        mkpi->mpi_total_size = tmp - (void*)msg;
        rc = mkpi->mpi_total_size + sizeof(*mkpi) + sizeof(int);
        return rc;
}

static int mds_rec_setattr_pack(char *buffer, struct dentry *dentry,
                                struct inode *dir, void *data1, void *data2)
{
        struct mds_rec_setattr *rec = NULL;
        struct mdc_op_data op_data;
        struct mds_kml_pack_info *mkpi;
        struct iattr *iattr = (struct iattr *)data1;
        struct lustre_msg *msg = NULL;
        char   *ea = NULL;
        void   *tmp = NULL;
        int    rc = 0, ealen = 0;

        ll_prepare_mdc_op_data(&op_data, dir, NULL, NULL, 0, 0);

        PACK_KML_REC_INIT(buffer, MDS_REINT);

        mkpi = (struct mds_kml_pack_info*)buffer;

        /*FIXME later, did not think about EA situation*/

        mkpi->mpi_bufcount = 1;
        mkpi->mpi_size[0] = sizeof(struct mds_rec_setattr);
        if (data2) {
                mkpi->mpi_bufcount ++;
                mkpi->mpi_size[1] = *(int *)data2;
                ealen = *(int *)data2;
                ea = data2 + sizeof(ealen);
        }

        msg = (struct lustre_msg *)(buffer + sizeof(*mkpi));
        lustre_init_msg(msg, mkpi->mpi_bufcount, mkpi->mpi_size, NULL);

        tmp = mdc_setattr_pack(msg, 0, &op_data, iattr, ea, ealen, NULL, 0);

        /*There are maybe some better ways
         *to set the time attr FIXME WANGDI later
         */
        rec = (struct mds_rec_setattr *)lustre_msg_buf(msg, 0, 0);
        if (rec->sa_valid & ATTR_CTIME)
                rec->sa_valid |= ATTR_CTIME_SET;
        if (rec->sa_valid & ATTR_MTIME)
                rec->sa_valid |= ATTR_MTIME_SET;
        if (rec->sa_valid & ATTR_ATIME)
                rec->sa_valid |= ATTR_ATIME_SET;

        mkpi->mpi_total_size = tmp - (void*)msg;
        rc = mkpi->mpi_total_size + sizeof(*mkpi) + sizeof(int);

        return rc;
}

static int mds_rec_create_pack(char *buffer, struct dentry *dentry,
                               struct inode *dir, void *data1, void *data2)
{
        struct lustre_msg *msg = NULL;
        struct mdc_op_data op_data;
        struct mds_rec_create *rec;
        struct mds_kml_pack_info *mkpi;
        void   *tmp = NULL;
        int    rc = 0, tgt_len = 0;

        ll_prepare_mdc_op_data(&op_data, dir, dentry->d_inode,
                               dentry->d_name.name, dentry->d_name.len, 0);
        PACK_KML_REC_INIT(buffer, MDS_REINT);

        mkpi = (struct mds_kml_pack_info*)buffer;
        mkpi->mpi_bufcount = 2;
        mkpi->mpi_size[0] = sizeof(struct mds_rec_create);
        mkpi->mpi_size[1] = op_data.namelen + 1;
        if (data1 && data2) {
                mkpi->mpi_size[2] = *(int *)data2;
                mkpi->mpi_bufcount++;
        }

        if (data1) {
                /*for symlink, data1 will be the tgt name */
                tgt_len = *(int*)data2;
        }
        msg = (struct lustre_msg *)(buffer + sizeof(*mkpi));
        lustre_init_msg(msg, mkpi->mpi_bufcount, mkpi->mpi_size, NULL);

        tmp = mdc_create_pack(msg, 0, &op_data, dentry->d_inode->i_mode,
                              dentry->d_inode->i_mode, data1, tgt_len);

        rec = (struct mds_rec_create *)lustre_msg_buf(msg, 0, 0);
        
        rec->cr_replayfid = op_data.fid2;
        rec->cr_flags |= REC_REINT_CREATE; 
        mkpi->mpi_total_size = tmp - (void*)msg;
        rc = mkpi->mpi_total_size + sizeof(*mkpi) + sizeof(int);

        return rc;
}

static int mds_rec_unlink_pack(char *buffer, struct dentry *dentry,
                               struct inode *dir, void *data1, void *data2)
{
        struct lustre_msg *msg = NULL;
        struct mdc_op_data op_data;
        struct mds_kml_pack_info *mkpi;
        int    mode = *(int*)data1;
        void   *tmp = NULL;
        int    rc = 0;

        ll_prepare_mdc_op_data(&op_data, dir, NULL, dentry->d_name.name,
                               dentry->d_name.len, mode);

        PACK_KML_REC_INIT(buffer, MDS_REINT);

        mkpi = (struct mds_kml_pack_info*)buffer;
        mkpi->mpi_bufcount = 2;
        mkpi->mpi_size[0] = sizeof(struct mds_rec_unlink);
        mkpi->mpi_size[1] = op_data.namelen + 1;

        msg = (struct lustre_msg *)(buffer + sizeof(*mkpi));
        lustre_init_msg(msg, mkpi->mpi_bufcount, mkpi->mpi_size, NULL);

        tmp = mdc_unlink_pack(msg, 0, &op_data);

        mkpi->mpi_total_size = tmp - (void*)msg;
        rc = mkpi->mpi_total_size + sizeof(*mkpi) + sizeof(int);

        return rc;
}

static int mds_rec_rename_pack(char *buffer, struct dentry *dentry,
                               struct inode *dir, void *data1, void *data2)
{
        struct lustre_msg *msg = NULL;
        struct dentry *new_dir = (struct dentry *)data1;
        struct dentry *new_dentry = (struct dentry *)data2;
        struct mdc_op_data op_data;
        struct mds_kml_pack_info *mkpi;
        struct mds_rec_rename *rec;
        void   *tmp = NULL;
        int    rc = 0;

        ll_prepare_mdc_op_data(&op_data, dir, new_dir->d_inode, NULL, 0, 0);

        PACK_KML_REC_INIT(buffer, MDS_REINT);

        mkpi = (struct mds_kml_pack_info*)buffer;
        mkpi->mpi_bufcount = 3;
        mkpi->mpi_size[0] = sizeof(struct mds_rec_rename);
        mkpi->mpi_size[1] = dentry->d_name.len + 1;
        mkpi->mpi_size[2] = new_dentry->d_name.len + 1;

        rec = (struct mds_rec_rename *)(buffer + sizeof(*mkpi));

        msg = (struct lustre_msg *)(buffer + sizeof(*mkpi));
        lustre_init_msg(msg, mkpi->mpi_bufcount, mkpi->mpi_size, NULL);

        tmp = mdc_rename_pack(msg, 0, &op_data, dentry->d_name.name,
                              dentry->d_name.len, new_dentry->d_name.name,
                              new_dentry->d_name.len);

        mkpi->mpi_total_size = tmp - (void*)msg;
        rc = mkpi->mpi_total_size + sizeof(*mkpi) + sizeof(int);
        return rc;
}

typedef int (*mds_pack_rec_func)(char *buffer, struct dentry *dentry,
                                 struct inode *dir, void *data1, void *data2);
static mds_pack_rec_func mds_kml_pack[REINT_MAX + 1] = {
        [REINT_LINK]    mds_rec_link_pack,
        [REINT_SETATTR] mds_rec_setattr_pack,
        [REINT_CREATE]  mds_rec_create_pack,
        [REINT_UNLINK]  mds_rec_unlink_pack,
        [REINT_RENAME]  mds_rec_rename_pack,
};

int mds_rec_pack(char *buffer, struct dentry *dentry, struct inode *dir,
                 void *data1, void *data2, int op)
{
        return mds_kml_pack[op](buffer, dentry, dir, data1, data2);
}

int mds_rec_pack_init(struct smfs_super_info *smsi)
{
        
        smsi->smsi_pack_rec[PACK_MDS] = mds_rec_pack;

        return 0;
}

