/*****************************************************************************

Copyright (c) 1996, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file trx/trx0rseg.cc
Rollback segment

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "trx0rseg.h"

#ifdef UNIV_NONINL
#include "trx0rseg.ic"
#endif

#include "trx0undo.h"
#include "fut0lst.h"
#include "srv0srv.h"
#include "trx0purge.h"
#include "srv0mon.h"
#include "fsp0sysspace.h"

#include <algorithm>

/** Creates a rollback segment header.
This function is called only when a new rollback segment is created in
the database.
@param[in]	space		space id
@param[in]	max_size	max size in pages
@param[in]	rseg_slot_no	rseg id == slot number in trx sys
@param[in,out]	mtr		mini-transaction
@return page number of the created segment, FIL_NULL if fail */
ulint
trx_rseg_header_create(
	ulint			space,
	ulint			max_size,
	ulint			rseg_slot_no,
	mtr_t*			mtr)
{
	ulint		page_no;
	trx_rsegf_t*	rsegf;
	trx_sysf_t*	sys_header;
	ulint		i;
	buf_block_t*	block;

	ut_ad(mtr);
	ut_ad(mtr_memo_contains(mtr, fil_space_get_latch(space, NULL),
				MTR_MEMO_X_LOCK));

	/* Allocate a new file segment for the rollback segment */
	block = fseg_create(space, 0, TRX_RSEG + TRX_RSEG_FSEG_HEADER, mtr);

	if (block == NULL) {
		/* No space left */

		return(FIL_NULL);
	}

	buf_block_dbg_add_level(block, SYNC_RSEG_HEADER_NEW);

	page_no = block->page.id.page_no();

	/* Get the rollback segment file page */
	rsegf = trx_rsegf_get_new(space, page_no, mtr);

	/* Initialize max size field */
	mlog_write_ulint(rsegf + TRX_RSEG_MAX_SIZE, max_size,
			 MLOG_4BYTES, mtr);

	/* Initialize the history list */

	mlog_write_ulint(rsegf + TRX_RSEG_HISTORY_SIZE, 0, MLOG_4BYTES, mtr);
	flst_init(rsegf + TRX_RSEG_HISTORY, mtr);

	/* Reset the undo log slots */
	for (i = 0; i < TRX_RSEG_N_SLOTS; i++) {

		trx_rsegf_set_nth_undo(rsegf, i, FIL_NULL, mtr);
	}

	if (!trx_sys_is_noredo_rseg_slot(rseg_slot_no)) {
		/* Non-redo rseg are re-created on restart and so no need
		to persist this information in sys-header. Anyway, on restart
		this information is not valid too as there is no space with
		persisted space-id on restart. */

		/* Add the rollback segment info to the free slot in
		the trx system header */

		sys_header = trx_sysf_get(mtr);

		trx_sysf_rseg_set_space(sys_header, rseg_slot_no, space, mtr);

		trx_sysf_rseg_set_page_no(
			sys_header, rseg_slot_no, page_no, mtr);
	}

	return(page_no);
}

/** Free a rollback segment in memory. */
void
trx_rseg_mem_free(trx_rseg_t* rseg)
{
	trx_undo_t*	undo;
	trx_undo_t*	next_undo;

	mutex_free(&rseg->mutex);

	/* There can't be any active transactions. */
	ut_a(UT_LIST_GET_LEN(rseg->undo_list) == 0);

	for (undo = UT_LIST_GET_FIRST(rseg->undo_cached);
	     undo != NULL;
	     undo = next_undo) {

		next_undo = UT_LIST_GET_NEXT(undo_list, undo);

		UT_LIST_REMOVE(rseg->undo_cached, undo);

		MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_CACHED);

		trx_undo_mem_free(undo);
	}

	ut_free(rseg);
}

/** Creates and initializes a rollback segment object.
The values for the fields are read from the header. The object is inserted to
the rseg list of the trx system object and a pointer is inserted in the rseg
array in the trx system object.
@param[in]	id		rollback segment id
@param[in]	space		space where the segment is placed
@param[in]	page_no		page number of the segment header
@param[in,out]	mtr		mini-transaction */
static
void
trx_rseg_mem_create(
	ulint			id,
	ulint			space,
	ulint			page_no,
	mtr_t*			mtr)
{
	ulint		len;
	trx_rseg_t*	rseg;
	fil_addr_t	node_addr;
	trx_rsegf_t*	rseg_header;
	trx_ulogf_t*	undo_log_hdr;
	ulint		sum_of_undo_sizes;

	rseg = static_cast<trx_rseg_t*>(ut_zalloc_nokey(sizeof(trx_rseg_t)));

	rseg->id = id;
	rseg->space = space;
	rseg->page_no = page_no;
	rseg->trx_ref_count = 0;
	rseg->skip_allocation = false;

	if (fsp_is_system_temporary(space)) {
		mutex_create(LATCH_ID_NOREDO_RSEG, &rseg->mutex);
	} else {
		mutex_create(LATCH_ID_REDO_RSEG, &rseg->mutex);
	}

	UT_LIST_INIT(rseg->undo_list, &trx_undo_t::undo_list);
	UT_LIST_INIT(rseg->undo_cached, &trx_undo_t::undo_list);

	trx_sys->rseg_array[id] = rseg;

	rseg_header = trx_rsegf_get_new(space, page_no, mtr);

	rseg->max_size = mtr_read_ulint(
		rseg_header + TRX_RSEG_MAX_SIZE, MLOG_4BYTES, mtr);

	/* Initialize the undo log lists according to the rseg header */

	sum_of_undo_sizes = trx_undo_lists_init(rseg);

	rseg->curr_size = mtr_read_ulint(
		rseg_header + TRX_RSEG_HISTORY_SIZE, MLOG_4BYTES, mtr)
		+ 1 + sum_of_undo_sizes;

	len = flst_get_len(rseg_header + TRX_RSEG_HISTORY);

	if (len > 0) {
		trx_sys->rseg_history_len += len;

		node_addr = trx_purge_get_log_from_hist(
			flst_get_last(rseg_header + TRX_RSEG_HISTORY, mtr));

		rseg->last_page_no = node_addr.page;
		rseg->last_offset = node_addr.boffset;

		undo_log_hdr = trx_undo_page_get(
			page_id_t(rseg->space, node_addr.page), mtr)
			+ node_addr.boffset;

		rseg->last_trx_no = mach_read_from_8(
			undo_log_hdr + TRX_UNDO_TRX_NO);

		rseg->last_del_marks = mtr_read_ulint(
			undo_log_hdr + TRX_UNDO_DEL_MARKS, MLOG_2BYTES, mtr);

		TrxUndoRsegs elem(rseg->last_trx_no);
		elem.push_back(rseg);

		if (rseg->last_page_no != FIL_NULL) {

			/* There is no need to cover this operation by the purge
			mutex because we are still bootstrapping. */

			purge_sys->purge_queue.push(elem);
		}
	} else {
		rseg->last_page_no = FIL_NULL;
	}
}

/** Initialize the rollback segments in memory at database startup. */
void
trx_rseg_array_init()
{
	mtr_t	mtr;

	for (ulint i = 0; i < TRX_SYS_N_RSEGS; i++) {
		ut_ad(!trx_rseg_get_on_id(i));
		mtr.start();
		trx_sysf_t*	sys_header = trx_sysf_get(&mtr);
		ulint		page_no = trx_sysf_rseg_get_page_no(
			sys_header, i, &mtr);

		if (page_no != FIL_NULL) {
			trx_rseg_mem_create(
				i,
				trx_sysf_rseg_get_space(sys_header, i, &mtr),
				page_no, &mtr);
		}

		mtr.commit();
	}
}

/*********************************************************************
Creates a rollback segment.
@return pointer to new rollback segment if create successful */
trx_rseg_t*
trx_rseg_create(
/*============*/
	ulint	space_id,	/*!< in: id of UNDO tablespace */
	ulint	nth_free_slot)	/*!< in: allocate nth free slot.
				0 means next free slots. */
{
	mtr_t		mtr;

	mtr.start();

	/* To obey the latching order, acquire the file space
	x-latch before the trx_sys->mutex. */
	const fil_space_t*	space = mtr_x_lock_space(space_id, &mtr);

	switch (space->purpose) {
	case FIL_TYPE_LOG:
	case FIL_TYPE_IMPORT:
		ut_ad(0);
	case FIL_TYPE_TEMPORARY:
		mtr.set_log_mode(MTR_LOG_NO_REDO);
		break;
	case FIL_TYPE_TABLESPACE:
		break;
	}

	ulint	slot_no = trx_sysf_rseg_find_free(
		&mtr, space->purpose == FIL_TYPE_TEMPORARY, nth_free_slot);
	ulint	page_no = slot_no == ULINT_UNDEFINED
		? FIL_NULL
		: trx_rseg_header_create(space_id, ULINT_MAX, slot_no, &mtr);

	if (page_no != FIL_NULL) {
		trx_sysf_t*	sys_header = trx_sysf_get(&mtr);

		ulint		id = trx_sysf_rseg_get_space(
			sys_header, slot_no, &mtr);
		ut_a(id == space_id || trx_sys_is_noredo_rseg_slot(slot_no));

		trx_rseg_mem_create(slot_no, space_id, page_no, &mtr);
	}

	mtr.commit();

	return(page_no == FIL_NULL ? NULL : trx_sys->rseg_array[slot_no]);
}

/********************************************************************
Get the number of unique rollback tablespaces in use except space id 0.
The last space id will be the sentinel value ULINT_UNDEFINED. The array
will be sorted on space id. Note: space_ids should have have space for
TRX_SYS_N_RSEGS + 1 elements.
@return number of unique rollback tablespaces in use. */
ulint
trx_rseg_get_n_undo_tablespaces(
/*============================*/
	ulint*		space_ids)	/*!< out: array of space ids of
					UNDO tablespaces */
{
	ulint		i;
	mtr_t		mtr;
	trx_sysf_t*	sys_header;
	ulint		n_undo_tablespaces = 0;

	mtr_start(&mtr);

	sys_header = trx_sysf_get(&mtr);

	for (i = 0; i < TRX_SYS_N_RSEGS; i++) {
		ulint	page_no;
		ulint	space;

		page_no = trx_sysf_rseg_get_page_no(sys_header, i, &mtr);

		if (page_no == FIL_NULL) {
			continue;
		}

		space = trx_sysf_rseg_get_space(sys_header, i, &mtr);

		if (space != 0) {
			ulint	j;
			ibool	found = FALSE;

			for (j = 0; j < n_undo_tablespaces; ++j) {
				if (space_ids[j] == space) {
					found = TRUE;
					break;
				}
			}

			if (!found) {
				ut_a(n_undo_tablespaces <= i);
				space_ids[n_undo_tablespaces++] = space;
			}
		}
	}

	mtr_commit(&mtr);

	ut_a(n_undo_tablespaces <= TRX_SYS_N_RSEGS);

	space_ids[n_undo_tablespaces] = ULINT_UNDEFINED;

	if (n_undo_tablespaces > 0) {
		std::sort(space_ids, space_ids + n_undo_tablespaces);
	}

	return(n_undo_tablespaces);
}
