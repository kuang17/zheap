/*-------------------------------------------------------------------------
 *
 * undodiscard.c
 *	  discard undo records
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/undo/undodiscard.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "access/undolog.h"
#include "access/undodiscard.h"
#include "catalog/pg_tablespace.h"
#include "miscadmin.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "storage/shmem.h"
#include "storage/proc.h"
#include "utils/resowner.h"
#include "postmaster/undoloop.h"

static UndoRecPtr FetchLatestUndoPtrForXid(UndoRecPtr urecptr,
										   UnpackedUndoRecord *uur_start,
										   UndoLogControl *log);

/*
 * Discard the undo for the log
 *
 * Search the undo log, get the start record for each transaction until we get
 * the transaction with xid >= xmin or an invalid xid.  Then call undolog
 * routine to discard upto that point and update the memory structure for the
 * log slot. We set the hibernate flag if we do not have any undo logs, this
 * flag is passed to the undo worker wherein it determines if system is idle
 * and it should sleep for sometime.
 *
 * Return the oldest xid remaining in this undo log (which should be >= xmin,
 * since we'll discard everything older).  Return InvalidTransactionId if the
 * undo log is empty.
 */
static TransactionId
UndoDiscardOneLog(UndoLogControl *log, TransactionId xmin, bool *hibernate)
{
	UndoRecPtr	undo_recptr, next_urecptr, from_urecptr, next_insert;
	UnpackedUndoRecord	*uur = NULL;
	bool	need_discard = false;
	TransactionId	undoxid = InvalidTransactionId;
	TransactionId	xid = log->oldest_xid;
	TransactionId	latest_discardxid = InvalidTransactionId;
	uint32	epoch = 0;

	undo_recptr = log->oldest_data;

	/* Loop until we run out of discardable transactions. */
	do
	{
		bool isCommitted, only_discard = false;

		next_insert = UndoLogGetNextInsertPtr(log->logno, xid);

		if (next_insert == undo_recptr)
		{
			if (undo_recptr == log->oldest_data)
			{
				/*
				 * If the discard location and the insert location is same then
				 * there is nothing to discard.
				 */
				break;
			}
			else
			{
				/*
				 * If the undo actions were already applied for a transaction
				 * and the undo record pointer is rewound, then undo worker
				 * only discards.
				 */
				only_discard = true;
				goto discard;
			}
		}

		/* Fetch the undo record for given undo_recptr. */
		uur = UndoFetchRecord(undo_recptr, InvalidBlockNumber,
							  InvalidOffsetNumber, InvalidTransactionId,
							  NULL, NULL);

		Assert(uur != NULL);

		isCommitted = TransactionIdDidCommit(uur->uur_xid);
		next_urecptr = uur->uur_next;
		undoxid = uur->uur_xid;
		xid = undoxid;
		epoch = uur->uur_xidepoch;

		/* There might not be any undo log and hibernation might be needed. */
		*hibernate = true;

		/*
		 * At system restart, undo actions need to be applied for all the
		 * transactions which were running the last time system was up. Now,
		 * the transactions which were running when the system was up and those
		 * that are active now are in-progress. To distinguish them we compare
		 * their respective xids to oldestxmin. Basically, the transactions
		 * with xid smaller than oldestxmin are the aborted ones. Hence,
		 * performing their undo actions.
		 */
		if (!isCommitted && TransactionIdPrecedes(undoxid, xmin))
		{
			/*
			 * At the time of recovery, we might not have a valid next undo
			 * record pointer and in that case we'll calculate the location
			 * of from pointer using the last record of next insert location.
			 */
			from_urecptr = FetchLatestUndoPtrForXid(undo_recptr, uur, log);
			UndoRecordRelease(uur);
			uur = NULL;

			StartTransactionCommand();
			execute_undo_actions(from_urecptr, undo_recptr, true, false, true);
			CommitTransactionCommand();

			/*
			 * Set the current resource owner to AuxProcessResourceOwner as
			 * CommitTransaction would have set it to NULL.  And we need it
			 * outside the transaction block for fetching the undo records.
			 */
			CurrentResourceOwner = AuxProcessResourceOwner;

		}

		/* we can discard upto this point. */
		if (TransactionIdFollowsOrEquals(undoxid, xmin) ||
			next_urecptr == SpecialUndoRecPtr ||
			UndoRecPtrGetLogNo(next_urecptr) != log->logno)
		{
discard:
			/* Hey, I got some undo log to discard, can not hibernate now. */
			*hibernate = false;

			if (uur && !only_discard)
				UndoRecordRelease(uur);

			/*
			 * If Transaction id is smaller than the xmin that means this must
			 * be the last transaction in this undo log, so we need to get the
			 * last insert point in this undo log and discard till that point.
			 * Also, if a transaction is aborted, we stop discarding undo from the
			 * same location.
			 */
			if (!only_discard && TransactionIdPrecedes(undoxid, xmin))
			{
				UndoRecPtr	next_insert = InvalidUndoRecPtr;

				/*
				 * Get the last insert location for this transaction Id, if it
				 * returns invalid pointer that means there is new transaction
				 * has started for this undolog.
				 */
				next_insert = UndoLogGetNextInsertPtr(log->logno, undoxid);

				if (!UndoRecPtrIsValid(next_insert))
					continue;

				undo_recptr = next_insert;
				need_discard = true;
				epoch = 0;
				latest_discardxid = undoxid;
				undoxid = InvalidTransactionId;
			}

			LWLockAcquire(&log->discard_lock, LW_EXCLUSIVE);
			if (only_discard)
			{
				log->oldest_xid = InvalidTransactionId;
				log->oldest_xidepoch = 0;
			}
			else
			{
				log->oldest_xid = undoxid;
				log->oldest_xidepoch = epoch;
			}
			log->oldest_data = undo_recptr;
			LWLockRelease(&log->discard_lock);

			if (need_discard)
				UndoLogDiscard(undo_recptr, latest_discardxid);

			break;
		}

		/*
		 * This transaction is smaller than the xmin so lets jump to the next
		 * transaction.
		 */
		undo_recptr = next_urecptr;
		latest_discardxid = undoxid;

		if(uur)
			UndoRecordRelease(uur);

		need_discard = true;

	} while (true);

	return undoxid;
}

/*
 * Discard the undo for all the transaction whose xid is smaller than xmin
 *
 *	Check the DiscardInfo memory array for each slot (every undo log) , process
 *	the undo log for all the slot which have xid smaller than xmin or invalid
 *	xid. Fetch the record from the undo log transaction by transaction until we
 *	find the xid which is not smaller than xmin.
 */
void
UndoDiscard(TransactionId oldestXmin, bool *hibernate)
{
	TransactionId	oldestXidHavingUndo = oldestXmin;
	uint64			epoch = GetEpochForXid(oldestXmin);
	UndoLogControl *log = NULL;

	/*
	 * TODO: Ideally we'd arrange undo logs so that we can efficiently find
	 * those with oldest_xid < oldestXmin, but for now we'll just scan all of
	 * them.
	 */
	while ((log = UndoLogNext(log)))
	{
		TransactionId oldest_xid = InvalidTransactionId;

		/* We can't process temporary undo logs. */
		if (log->meta.persistence == UNDO_TEMP)
			continue;

		/*
		 * If the first xid of the undo log is smaller than the xmin the try
		 * to discard the undo log.
		 */
		if (TransactionIdPrecedes(log->oldest_xid, oldestXmin))
		{
			/*
			 * If the XID in the discard entry is invalid then start scanning from
			 * the first valid undorecord in the log.
			 */
			if (!TransactionIdIsValid(log->oldest_xid))
			{
				UndoRecPtr urp = UndoLogGetFirstValidRecord(log->logno);

				if (!UndoRecPtrIsValid(urp))
					continue;

				LWLockAcquire(&log->discard_lock, LW_SHARED);
				log->oldest_data = urp;
				LWLockRelease(&log->discard_lock);
			}

			/* Process the undo log. */
			oldest_xid = UndoDiscardOneLog(log, oldestXmin, hibernate);
		}

		if (TransactionIdIsValid(oldest_xid) &&
			TransactionIdPrecedes(oldest_xid, oldestXidHavingUndo))
		{
			oldestXidHavingUndo = oldest_xid;
			epoch = GetEpochForXid(oldest_xid);
		}
	}

	/*
	 * Update the oldestXidWithEpochHavingUndo in the shared memory.
	 *
	 * XXX In future if multiple worker can perform discard then we may need
	 * to use compare and swap for updating the shared memory value.
	 */
	pg_atomic_write_u64(&ProcGlobal->oldestXidWithEpochHavingUndo,
						MakeEpochXid(epoch, oldestXidHavingUndo));
}

/*
 * Fetch the latest urec pointer for the transaction.
 */
UndoRecPtr
FetchLatestUndoPtrForXid(UndoRecPtr urecptr, UnpackedUndoRecord *uur_start,
						 UndoLogControl *log)
{
	UndoRecPtr next_urecptr, from_urecptr;
	uint16	prevlen;
	UndoLogOffset next_insert;
	UnpackedUndoRecord *uur;
	bool refetch = false;

	uur = uur_start;

	while (true)
	{
		/* fetch the undo record again if required. */
		if (refetch)
		{
			uur = UndoFetchRecord(urecptr, InvalidBlockNumber,
								  InvalidOffsetNumber, InvalidTransactionId,
								  NULL, NULL);
			refetch = false;
		}

		next_urecptr = uur->uur_next;
		prevlen = UndoLogGetPrevLen(log->logno);

		/*
		 * If this is the last transaction in the log then calculate the latest
		 * urec pointer using next insert location of the undo log.  Otherwise,
		 * calculate using next transaction's start pointer.
		 */
		if (uur->uur_next == SpecialUndoRecPtr)
		{
			/*
			 * While fetching the next insert location if the new transaction
			 * has already started in this log then lets re-fetch the undo
			 * record.
			 */
			next_insert = UndoLogGetNextInsertPtr(log->logno, uur->uur_xid);
			if (!UndoRecPtrIsValid(next_insert))
			{
				if (uur != uur_start)
					UndoRecordRelease(uur);
				refetch = true;
				continue;
			}

			from_urecptr = UndoGetPrevUndoRecptr(next_insert, prevlen);
			break;
		}
		else if ((UndoRecPtrGetLogNo(next_urecptr) != log->logno) &&
				UndoLogIsDiscarded(next_urecptr))
		{
			/*
			 * If next_urecptr is in different undolog and its already discarded
			 * that means the undo actions for this transaction which are in the
			 * next log has already been executed and we only need to execute
			 * which are remaining in this log.
			 */
			next_insert = UndoLogGetNextInsertPtr(log->logno, uur->uur_xid);

			Assert(UndoRecPtrIsValid(next_insert));
			from_urecptr = UndoGetPrevUndoRecptr(next_insert, prevlen);
			break;
		}
		else
		{
			UnpackedUndoRecord	*next_uur;

			next_uur = UndoFetchRecord(next_urecptr,
										InvalidBlockNumber,
										InvalidOffsetNumber,
										InvalidTransactionId,
										NULL, NULL);
			/*
			 * If the next_urecptr is in the same log then calculate the
			 * from pointer using prevlen.
			 */
			if (UndoRecPtrGetLogNo(next_urecptr) == log->logno)
			{
				from_urecptr =
					UndoGetPrevUndoRecptr(next_urecptr, next_uur->uur_prevlen);
				UndoRecordRelease(next_uur);
				break;
			}
			else
			{
				/*
				 * The transaction is overflowed to the next log, so restart
				 * the processing from then next log.
				 */
				log = UndoLogGet(UndoRecPtrGetLogNo(next_urecptr));
				if (uur != uur_start)
					UndoRecordRelease(uur);
				uur = next_uur;
				continue;
			}

			UndoRecordRelease(next_uur);
		}
	}

	if (uur != uur_start)
		UndoRecordRelease(uur);

	return from_urecptr;
}

/*
 * Discard the undo logs for temp tables.
 */
void
TempUndoDiscard(UndoLogNumber logno)
{
	UndoLogControl *log = UndoLogGet(logno);

	/*
	 * Discard the undo log for temp table only. Ensure that there is
	 * something to be discarded there.
	 */
	Assert (log->meta.persistence == UNDO_TEMP);

	/* Process the undo log. */
	UndoLogDiscard(MakeUndoRecPtr(log->logno, log->meta.insert),
				   InvalidTransactionId);
}
