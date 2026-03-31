/*-------------------------------------------------------------------------
 *
 * nodeSeqscan.c
 *	  Support routines for sequential scans of relations.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeSeqscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecSeqScan				sequentially scans a relation.
 *		ExecSeqNext				retrieve next tuple in sequential order.
 *		ExecInitSeqScan			creates and initializes a seqscan node.
 *		ExecEndSeqScan			releases any storage allocated.
 *		ExecReScanSeqScan		rescans the relation
 *
 *		ExecSeqScanEstimate		estimates DSM space needed for parallel scan
 *		ExecSeqScanInitializeDSM initialize DSM for parallel scan
 *		ExecSeqScanReInitializeDSM reinitialize DSM for fresh parallel scan
 *		ExecSeqScanInitializeWorker attach to DSM info in parallel worker
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/tableam.h"
#include "executor/execScan.h"
#include "executor/executor.h"
#include "executor/nodeSeqscan.h"
#include "utils/rel.h"

static TupleTableSlot *SeqNext(SeqScanState *node);

/* ----------------------------------------------------------------
 *						Scan Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		SeqNext
 *
 *		This is a workhorse for ExecSeqScan
 * ----------------------------------------------------------------
 */
static pg_attribute_always_inline TupleTableSlot *
SeqNext(SeqScanState *node)
{
	TableScanDesc scandesc;
	EState	   *estate;
	ScanDirection direction;
	TupleTableSlot *slot;

	/*
	 * get information from the estate and scan state
	 */
	scandesc = node->ss.ss_currentScanDesc;
	estate = node->ss.ps.state;
	direction = estate->es_direction;
	slot = node->ss.ss_ScanTupleSlot;

	if (scandesc == NULL)
	{
		uint32	flags = SO_NONE;

		if (ScanRelIsReadOnly(&node->ss))
			flags |= SO_HINT_REL_READ_ONLY;

		if (estate->es_instrument)
			flags |= SO_SCAN_INSTRUMENT;

		/*
		 * We reach here if the scan is not parallel, or if we're serially
		 * executing a scan that was planned to be parallel.
		 */
		scandesc = table_beginscan(node->ss.ss_currentRelation,
								   estate->es_snapshot,
								   0, NULL,
								   flags);
		node->ss.ss_currentScanDesc = scandesc;
	}

	/*
	 * get the next tuple from the table
	 */
	if (table_scan_getnextslot(scandesc, direction, slot))
		return slot;
	return NULL;
}

/*
 * SeqRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static pg_attribute_always_inline bool
SeqRecheck(SeqScanState *node, TupleTableSlot *slot)
{
	/*
	 * Note that unlike IndexScan, SeqScan never use keys in heap_beginscan
	 * (and this is very bad) - so, here we do not check are keys ok or not.
	 */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecSeqScan(node)
 *
 *		Scans the relation sequentially and returns the next qualifying
 *		tuple. This variant is used when there is no es_epq_active, no qual
 *		and no projection.  Passing const-NULLs for these to ExecScanExtended
 *		allows the compiler to eliminate the additional code that would
 *		ordinarily be required for the evaluation of these.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecSeqScan(PlanState *pstate)
{
	SeqScanState *node = castNode(SeqScanState, pstate);

	Assert(pstate->state->es_epq_active == NULL);
	Assert(pstate->qual == NULL);
	Assert(pstate->ps_ProjInfo == NULL);

	return ExecScanExtended(&node->ss,
							(ExecScanAccessMtd) SeqNext,
							(ExecScanRecheckMtd) SeqRecheck,
							NULL,
							NULL,
							NULL);
}

/*
 * Variant of ExecSeqScan() but when qual evaluation is required.
 */
static TupleTableSlot *
ExecSeqScanWithQual(PlanState *pstate)
{
	SeqScanState *node = castNode(SeqScanState, pstate);

	/*
	 * Use pg_assume() for != NULL tests to make the compiler realize no
	 * runtime check for the field is needed in ExecScanExtended().
	 */
	Assert(pstate->state->es_epq_active == NULL);
	pg_assume(pstate->qual != NULL);
	Assert(pstate->ps_ProjInfo == NULL);

	return ExecScanExtended(&node->ss,
							(ExecScanAccessMtd) SeqNext,
							(ExecScanRecheckMtd) SeqRecheck,
							NULL,
							pstate->qual,
							NULL);
}

/*
 * Variant of ExecSeqScan() but when projection is required.
 */
static TupleTableSlot *
ExecSeqScanWithProject(PlanState *pstate)
{
	SeqScanState *node = castNode(SeqScanState, pstate);

	Assert(pstate->state->es_epq_active == NULL);
	Assert(pstate->qual == NULL);
	pg_assume(pstate->ps_ProjInfo != NULL);

	return ExecScanExtended(&node->ss,
							(ExecScanAccessMtd) SeqNext,
							(ExecScanRecheckMtd) SeqRecheck,
							NULL,
							NULL,
							pstate->ps_ProjInfo);
}

/*
 * Variant of ExecSeqScan() but when qual evaluation and projection are
 * required.
 */
static TupleTableSlot *
ExecSeqScanWithQualProject(PlanState *pstate)
{
	SeqScanState *node = castNode(SeqScanState, pstate);

	Assert(pstate->state->es_epq_active == NULL);
	pg_assume(pstate->qual != NULL);
	pg_assume(pstate->ps_ProjInfo != NULL);

	return ExecScanExtended(&node->ss,
							(ExecScanAccessMtd) SeqNext,
							(ExecScanRecheckMtd) SeqRecheck,
							NULL,
							pstate->qual,
							pstate->ps_ProjInfo);
}

/*
 * Variant of ExecSeqScan for when EPQ evaluation is required.  We don't
 * bother adding variants of this for with/without qual and projection as
 * EPQ doesn't seem as exciting a case to optimize for.
 */
static TupleTableSlot *
ExecSeqScanEPQ(PlanState *pstate)
{
	SeqScanState *node = castNode(SeqScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) SeqNext,
					(ExecScanRecheckMtd) SeqRecheck);
}

/* ----------------------------------------------------------------
 *		ExecInitSeqScan
 * ----------------------------------------------------------------
 */
SeqScanState *
ExecInitSeqScan(SeqScan *node, EState *estate, int eflags)
{
	SeqScanState *scanstate;

	/*
	 * Once upon a time it was possible to have an outerPlan of a SeqScan, but
	 * not any more.
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create state structure
	 */
	scanstate = makeNode(SeqScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * open the scan relation
	 */
	scanstate->ss.ss_currentRelation =
		ExecOpenScanRelation(estate,
							 node->scan.scanrelid,
							 eflags);

	/* and create slot with the appropriate rowtype */
	ExecInitScanTupleSlot(estate, &scanstate->ss,
						  RelationGetDescr(scanstate->ss.ss_currentRelation),
						  table_slot_callbacks(scanstate->ss.ss_currentRelation),
						  TTS_FLAG_OBEYS_NOT_NULL_CONSTRAINTS);

	/*
	 * Initialize result type and projection.
	 */
	ExecInitResultTypeTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	/*
	 * initialize child expressions
	 */
	scanstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);

	/*
	 * When EvalPlanQual() is not in use, assign ExecProcNode for this node
	 * based on the presence of qual and projection. Each ExecSeqScan*()
	 * variant is optimized for the specific combination of these conditions.
	 */
	if (scanstate->ss.ps.state->es_epq_active != NULL)
		scanstate->ss.ps.ExecProcNode = ExecSeqScanEPQ;
	else if (scanstate->ss.ps.qual == NULL)
	{
		if (scanstate->ss.ps.ps_ProjInfo == NULL)
			scanstate->ss.ps.ExecProcNode = ExecSeqScan;
		else
			scanstate->ss.ps.ExecProcNode = ExecSeqScanWithProject;
	}
	else
	{
		if (scanstate->ss.ps.ps_ProjInfo == NULL)
			scanstate->ss.ps.ExecProcNode = ExecSeqScanWithQual;
		else
			scanstate->ss.ps.ExecProcNode = ExecSeqScanWithQualProject;
	}

	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndSeqScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndSeqScan(SeqScanState *node)
{
	TableScanDesc scanDesc;

	/*
	 * When ending a parallel worker, copy the statistics gathered by the
	 * worker back into shared memory so that it can be picked up by the main
	 * process to report in EXPLAIN ANALYZE.
	 */
	if (node->sinstrument != NULL && IsParallelWorker())
	{
		SeqScanInstrumentation *si;

		Assert(ParallelWorkerNumber < node->sinstrument->num_workers);
		si = &node->sinstrument->sinstrument[ParallelWorkerNumber];

		/*
		 * Here we accumulate the stats rather than performing memcpy on
		 * node->stats into si.  When a Gather/GatherMerge node finishes it
		 * will perform planner shutdown on the workers.  On rescan it will
		 * spin up new workers which will have a new SeqScanState and
		 * zeroed stats.
		 */

		/* collect prefetch info for this process from the read_stream */
		if (node->ss.ss_currentScanDesc &&
			node->ss.ss_currentScanDesc->rs_instrument)
		{
			AccumulateIOStats(&si->stats.io,
							  &node->ss.ss_currentScanDesc->rs_instrument->io);
		}
	}

	/*
	 * get information from node
	 */
	scanDesc = node->ss.ss_currentScanDesc;

	/*
	 * close heap scan
	 */
	if (scanDesc != NULL)
		table_endscan(scanDesc);
}

/* ----------------------------------------------------------------
 *						Join Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecReScanSeqScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecReScanSeqScan(SeqScanState *node)
{
	TableScanDesc scan;

	scan = node->ss.ss_currentScanDesc;

	if (scan != NULL)
		table_rescan(scan,		/* scan desc */
					 NULL);		/* new scan keys */

	ExecScanReScan((ScanState *) node);
}

/* ----------------------------------------------------------------
 *						Parallel Scan Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecSeqScanEstimate
 *
 *		Compute the amount of space we'll need in the parallel
 *		query DSM, and inform pcxt->estimator about our needs.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanEstimate(SeqScanState *node,
					ParallelContext *pcxt)
{
	EState	   *estate = node->ss.ps.state;
	bool		instrument = node->ss.ps.instrument != NULL;
	bool		parallel_aware = node->ss.ps.plan->parallel_aware;
	Size		size;

	if (!instrument && !parallel_aware)
	{
		/* No DSM required by the scan */
		return;
	}

	size = table_parallelscan_estimate(node->ss.ss_currentRelation,
									   estate->es_snapshot);
	node->pscan_len = size;

	/* make sure the instrumentation is properly aligned */
	size = MAXALIGN(size);

	/* account for instrumentation, if required */
	if (instrument && pcxt->nworkers > 0)
	{
		size = add_size(size, offsetof(SharedSeqScanInstrumentation, sinstrument));
		size = add_size(size, mul_size(pcxt->nworkers, sizeof(SeqScanInstrumentation)));
	}

	shm_toc_estimate_chunk(&pcxt->estimator, size);
	shm_toc_estimate_keys(&pcxt->estimator, 1);
}

/* ----------------------------------------------------------------
 *		ExecSeqScanInitializeDSM
 *
 *		Set up a parallel heap scan descriptor.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanInitializeDSM(SeqScanState *node,
						 ParallelContext *pcxt)
{
	EState	   *estate = node->ss.ps.state;
	ParallelTableScanDesc pscan;
	SharedSeqScanInstrumentation *sinstrument = NULL;
	bool		instrument = node->ss.ps.instrument != NULL;
	bool		parallel_aware = node->ss.ps.plan->parallel_aware;
	Size		size;
	uint32		flags = SO_NONE;

	if (!instrument && !parallel_aware)
	{
		/* No DSM required by the scan */
		return;
	}

	/* Recalculate the size. This needs to match ExecSeqScanEstimate. */
	size = MAXALIGN(node->pscan_len);
	if (instrument && pcxt->nworkers > 0)
	{
		size = add_size(size, offsetof(SharedSeqScanInstrumentation, sinstrument));
		size = add_size(size, mul_size(pcxt->nworkers, sizeof(SeqScanInstrumentation)));
	}

	pscan = shm_toc_allocate(pcxt->toc, size);
	pscan->phs_len = node->pscan_len;

	table_parallelscan_initialize(node->ss.ss_currentRelation,
								  pscan,
								  estate->es_snapshot);

	shm_toc_insert(pcxt->toc, node->ss.ps.plan->plan_node_id, pscan);

	/* initialize the shared instrumentation (with correct alignment) */
	if (instrument && pcxt->nworkers > 0)
	{
		char *ptr = (char *) pscan;

		ptr += MAXALIGN(node->pscan_len);

		sinstrument = (SharedSeqScanInstrumentation *) ptr;

		sinstrument->num_workers = pcxt->nworkers;

		/* ensure any unfilled slots will contain zeroes */
		memset(sinstrument->sinstrument, 0,
			   pcxt->nworkers * sizeof(SeqScanInstrumentation));

		node->sinstrument = sinstrument;
	}

	if (!parallel_aware)
	{
		/* Only here to set up worker node's shared instrumentation */
		return;
	}

	if (ScanRelIsReadOnly(&node->ss))
		flags |= SO_HINT_REL_READ_ONLY;

	if (estate->es_instrument)
		flags |= SO_SCAN_INSTRUMENT;

	node->ss.ss_currentScanDesc =
		table_beginscan_parallel(node->ss.ss_currentRelation, pscan, flags);
}

/* ----------------------------------------------------------------
 *		ExecSeqScanReInitializeDSM
 *
 *		Reset shared state before beginning a fresh scan.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanReInitializeDSM(SeqScanState *node,
						   ParallelContext *pcxt)
{
	ParallelTableScanDesc pscan;

	pscan = node->ss.ss_currentScanDesc->rs_parallel;
	table_parallelscan_reinitialize(node->ss.ss_currentRelation, pscan);
}

/* ----------------------------------------------------------------
 *		ExecSeqScanInitializeWorker
 *
 *		Copy relevant information from TOC into planstate.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanInitializeWorker(SeqScanState *node,
							ParallelWorkerContext *pwcxt)
{
	ParallelTableScanDesc pscan;
	EState	   *estate = node->ss.ps.state;
	bool		instrument = node->ss.ps.instrument != NULL;
	bool		parallel_aware = node->ss.ps.plan->parallel_aware;
	uint32		flags = SO_NONE;

	if (!instrument && !parallel_aware)
	{
		/* No DSM required by the scan */
		return;
	}

	pscan = shm_toc_lookup(pwcxt->toc, node->ss.ps.plan->plan_node_id, false);

	/* set pointer to the shared instrumentation */
	if (instrument)
	{
		char *ptr = (char *) pscan;
		ptr += MAXALIGN(pscan->phs_len);

		node->sinstrument = (SharedSeqScanInstrumentation *) ptr;
	}

	if (!parallel_aware)
	{
		/* Only here to set up worker node's SharedInfo */
		return;
	}

	if (ScanRelIsReadOnly(&node->ss))
		flags |= SO_HINT_REL_READ_ONLY;

	if (estate->es_instrument)
		flags |= SO_SCAN_INSTRUMENT;

	node->ss.ss_currentScanDesc =
		table_beginscan_parallel(node->ss.ss_currentRelation, pscan, flags);
}

/* ----------------------------------------------------------------
 *		ExecSeqScanRetrieveInstrumentation
 *
 *		Transfer seq scan statistics from DSM to private memory.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanRetrieveInstrumentation(SeqScanState *node)
{
	SharedSeqScanInstrumentation *sinstrument = node->sinstrument;
	Size		size;

	if (sinstrument == NULL)
		return;

	size = offsetof(SharedSeqScanInstrumentation, sinstrument)
		+ sinstrument->num_workers * sizeof(SeqScanInstrumentation);

	node->sinstrument = palloc(size);
	memcpy(node->sinstrument, sinstrument, size);
}
