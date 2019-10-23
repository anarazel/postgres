/*-------------------------------------------------------------------------
 *
 * execExpr.c
 *	  Expression evaluation infrastructure.
 *
 *	During executor startup, we compile each expression tree (which has
 *	previously been processed by the parser and planner) into an ExprState,
 *	using ExecInitExpr() et al.  This converts the tree into a flat array
 *	of ExprEvalSteps, which may be thought of as instructions in a program.
 *	At runtime, we'll execute steps, starting with the first, until we reach
 *	an EEOP_DONE opcode.
 *
 *	This file contains the "compilation" logic.  It is independent of the
 *	specific execution technology we use (switch statement, computed goto,
 *	JIT compilation, etc).
 *
 *	See src/backend/executor/README for some background, specifically the
 *	"Expression Trees and ExprState nodes", "Expression Initialization",
 *	and "Expression Evaluation" sections.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execExpr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_type.h"
#include "executor/execExpr.h"
#include "executor/nodeSubplan.h"
#include "funcapi.h"
#include "jit/jit.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"


typedef struct LastAttnumInfo
{
	AttrNumber	last_inner;
	AttrNumber	last_outer;
	AttrNumber	last_scan;
} LastAttnumInfo;

static void* ExecReadyExprEmbedded(ExprStateBuilder *esb, size_t prefix);
static ExprState* ExecReadyExpr(ExprStateBuilder *esb);
static void ExecInitExprRec(ExprStateBuilder *esb,
							Expr *node, RelNullableDatum result);
static void ExecInitFunc(ExprEvalStep *scratch, Expr *node, List *args,
						 Oid funcid, Oid inputcollid,
						 ExprStateBuilder *esb);
static void ExecInitExprSlots(ExprStateBuilder *esb, Node *node);
static void ExprEvalPushStep(ExprStateBuilder *esb, const ExprEvalStep *s);
static void ExecPushExprSlots(ExprStateBuilder *esb, LastAttnumInfo *info);
static bool get_last_attnums_walker(Node *node, LastAttnumInfo *info);
static bool ExecComputeSlotInfo(ExprStateBuilder *esb, ExprEvalStep *op);
static void ExecInitWholeRowVar(ExprEvalStep *scratch, Var *variable,
								ExprStateBuilder *esb);
static void ExecInitSubscriptingRef(ExprEvalStep *scratch,
									SubscriptingRef *sbsref,
									ExprStateBuilder *esb,
									RelNullableDatum result);
static bool isAssignmentIndirectionExpr(Expr *expr);
static void ExecInitCoerceToDomain(ExprEvalStep *scratch, CoerceToDomain *ctest,
								   ExprStateBuilder *esb,
								   RelNullableDatum result);
static void ExecBuildAggTransCall(ExprStateBuilder *esb, AggState *aggstate,
								  ExprEvalStep *scratch,
								  FunctionCallInfo fcinfo, RelFunctionCallInfo fcinfo_rel,
								  RelNullableDatumArray columns,
								  AggStatePerTrans pertrans,
								  int transno, int setno, int setoff, bool ishash);

static ExprStateAllocation *
ExprBuilderAllocInternal(ExprStateBuilder *esb, ExprRelPtrKind kind, uint32 sz)
{
	ExprStateAllocation *alloc = palloc(sizeof(ExprStateAllocation));

	// FIXME: should allow other alignment
	esb->allocation_size = MAXALIGN(esb->allocation_size);

	alloc->ptr.off = esb->allocation_size;
	alloc->ptr.allocno = list_length(esb->allocations) + 1;
	alloc->sz = sz;
	alloc->kind = kind;
	alloc->zeroed = false;
	alloc->initial_content = NULL;

	esb->allocation_size += sz;
	esb->allocations = lappend(esb->allocations, alloc);

	return alloc;
}

static RelNullableDatum
ExprBuilderAllocNullableDatum(ExprStateBuilder *esb)
{
	ExprStateAllocation *alloc =
		ExprBuilderAllocInternal(esb, ERP_NULLABLE_DATUM, sizeof(NullableDatum));

	return (RelNullableDatum){alloc->ptr};
}

static RelBool
ExprBuilderAllocBool(ExprStateBuilder *esb)
{
	ExprStateAllocation *alloc = ExprBuilderAllocInternal(esb, ERP_BOOL, sizeof(NullableDatum));

	return (RelBool){alloc->ptr};
}

static RelNullableDatumArray
ExprBuilderAllocNullableDatumArray(ExprStateBuilder *esb, int nelems)
{
	size_t sz = sizeof(NullableDatum) * nelems;
	ExprStateAllocation *alloc;

	alloc = ExprBuilderAllocInternal(esb, ERP_NULLABLE_DATUM_ARRAY, sz);

	return (RelNullableDatumArray){alloc->ptr};
}

static RelNullableDatumArray
ExprBuilderAllocNullableDatumArrayInit(ExprStateBuilder *esb, int nelems, NullableDatum **elements)
{
	size_t sz = sizeof(NullableDatum) * nelems;
	ExprStateAllocation *alloc;

	alloc = ExprBuilderAllocInternal(esb, ERP_NULLABLE_DATUM_ARRAY, sz);

	alloc->zeroed = true;
	alloc->initial_content = palloc0(sz);
	*elements = (NullableDatum *) alloc->initial_content;

	return (RelNullableDatumArray){alloc->ptr};
}

static RelFunctionCallInfo
ExprBuilderAllocFunctionCallInfo(ExprStateBuilder *esb, int nargs, FunctionCallInfo *ptr)
{
	ExprStateAllocation *alloc;
	Size sz = SizeForFunctionCallInfo(nargs);

	alloc = ExprBuilderAllocInternal(esb, ERP_FUNCTIONCALLINFO, sz);

	alloc->zeroed = true;
	alloc->initial_content = palloc0(sz);
	*ptr = alloc->initial_content;

	return (RelFunctionCallInfo){alloc->ptr};
}

static RelNullableDatum
RelArrayIdx(RelNullableDatumArray arr, int idx)
{
	return (RelNullableDatum){{
			.allocno = arr.ptr.allocno,
			.off = arr.ptr.off + sizeof(NullableDatum) * idx}};
}

static RelNullableDatum
RelFCIIdx(RelFunctionCallInfo arr, int idx)
{
	return (RelNullableDatum){{
			.allocno = arr.ptr.allocno,
			.off = arr.ptr.off + offsetof(FunctionCallInfoBaseData, args[idx])}};
}

static RelNullableDatumArray
RelFCIOff(RelFunctionCallInfo arr, int idx)
{
	return (RelNullableDatumArray){{
			.allocno = arr.ptr.allocno,
			.off = arr.ptr.off + offsetof(FunctionCallInfoBaseData, args[idx])}};
}


static ExprEvalStep*
nth_step(ExprStateBuilder *esb, int n)
{
	Assert(n < list_length(esb->steps));

	return (ExprEvalStep *) list_nth(esb->steps, n);
}


/*
 * ExecInitExpr: prepare an expression tree for execution
 *
 * This function builds and returns an ExprState implementing the given
 * Expr node tree.  The return ExprState can then be handed to ExecEvalExpr
 * for execution.  Because the Expr tree itself is read-only as far as
 * ExecInitExpr and ExecEvalExpr are concerned, several different executions
 * of the same plan tree can occur concurrently.  (But note that an ExprState
 * does mutate at runtime, so it can't be re-used concurrently.)
 *
 * This must be called in a memory context that will last as long as repeated
 * executions of the expression are needed.  Typically the context will be
 * the same as the per-query context of the associated ExprContext.
 *
 * Any Aggref, WindowFunc, or SubPlan nodes found in the tree are added to
 * the lists of such nodes held by the parent PlanState (or more accurately,
 * the AggrefExprState etc. nodes created for them are added).
 *
 * Note: there is no ExecEndExpr function; we assume that any resource
 * cleanup needed will be handled by just releasing the memory context
 * in which the state tree is built.  Functions that require additional
 * cleanup work can register a shutdown callback in the ExprContext.
 *
 *	'node' is the root of the expression tree to compile.
 *	'parent' is the PlanState node that owns the expression.
 *
 * 'parent' may be NULL if we are preparing an expression that is not
 * associated with a plan tree.  (If so, it can't have aggs or subplans.)
 * Such cases should usually come through ExecPrepareExpr, not directly here.
 *
 * Also, if 'node' is NULL, we just return NULL.  This is convenient for some
 * callers that may or may not have an expression that needs to be compiled.
 * Note that a NULL ExprState pointer *cannot* be handed to ExecEvalExpr,
 * although ExecQual and ExecCheck will accept one (and treat it as "true").
 */
ExprState *
ExecInitExpr(Expr *node, PlanState *parent)
{
	ExprStateBuilder esb = {0};
	ExprEvalStep scratch = {0};

	/* Special case: NULL expression produces a NULL ExprState pointer */
	if (node == NULL)
		return NULL;

	esb.expr = node;
	esb.parent = parent;
	esb.ext_params = NULL;
	esb.result = ExprBuilderAllocNullableDatum(&esb);

	/* Insert EEOP_*_FETCHSOME steps as needed */
	ExecInitExprSlots(&esb, (Node *) node);

	/* Compile the expression proper */
	ExecInitExprRec(&esb, node, esb.result);

	/* Finally, append a DONE step */
	scratch.opcode = EEOP_DONE_RETURN;
	scratch.result = esb.result;
	ExprEvalPushStep(&esb, &scratch);

	return ExecReadyExpr(&esb);
}

/*
 * ExecInitExprWithParams: prepare a standalone expression tree for execution
 *
 * This is the same as ExecInitExpr, except that there is no parent PlanState,
 * and instead we may have a ParamListInfo describing PARAM_EXTERN Params.
 *
 * FIXME: don't duplicate?
 */
ExprState *
ExecInitExprWithParams(Expr *node, ParamListInfo ext_params)
{
	ExprStateBuilder esb = {0};
	ExprEvalStep scratch = {0};

	/* Special case: NULL expression produces a NULL ExprState pointer */
	if (node == NULL)
		return NULL;

	/* Initialize ExprState with empty step list */
	esb.expr = node;
	esb.parent = NULL;
	esb.ext_params = ext_params;

	esb.result = ExprBuilderAllocNullableDatum(&esb);

	/* Insert EEOP_*_FETCHSOME steps as needed */
	ExecInitExprSlots(&esb, (Node *) node);

	/* Compile the expression proper */
	ExecInitExprRec(&esb, node, esb.result);

	/* Finally, append a DONE step */
	scratch.opcode = EEOP_DONE_RETURN;
	scratch.result = esb.result;
	ExprEvalPushStep(&esb, &scratch);

	return ExecReadyExpr(&esb);
}

/*
 * ExecInitQual: prepare a qual for execution by ExecQual
 *
 * Prepares for the evaluation of a conjunctive boolean expression (qual list
 * with implicit AND semantics) that returns true if none of the
 * subexpressions are false.
 *
 * We must return true if the list is empty.  Since that's a very common case,
 * we optimize it a bit further by translating to a NULL ExprState pointer
 * rather than setting up an ExprState that computes constant TRUE.  (Some
 * especially hot-spot callers of ExecQual detect this and avoid calling
 * ExecQual at all.)
 *
 * If any of the subexpressions yield NULL, then the result of the conjunction
 * is false.  This makes ExecQual primarily useful for evaluating WHERE
 * clauses, since SQL specifies that tuples with null WHERE results do not
 * get selected.
 */
ExprState *
ExecInitQual(List *qual, PlanState *parent)
{
	ExprStateBuilder esb = {0};
	ExprEvalStep scratch = {0};
	List	   *adjust_jumps = NIL;
	ListCell   *lc;

	/* short-circuit (here and in ExecQual) for empty restriction list */
	if (qual == NIL)
		return NULL;

	Assert(IsA(qual, List));

	esb.expr = (Expr *) qual;
	esb.parent = parent;
	esb.ext_params = NULL;
	/* Use a temporary as target for each qual expr. */
	esb.result = ExprBuilderAllocNullableDatum(&esb);

	/* mark expression as to be used with ExecQual() */
	esb.flags = EEO_FLAG_IS_QUAL;

	/* Insert EEOP_*_FETCHSOME steps as needed */
	ExecInitExprSlots(&esb, (Node *) qual);

	/*
	 * ExecQual() needs to return false for an expression returning NULL. That
	 * allows us to short-circuit the evaluation the first time a NULL is
	 * encountered.  As qual evaluation is a hot-path this warrants using a
	 * special opcode for qual evaluation that's simpler than BOOL_AND (which
	 * has more complex NULL handling).
	 */
	scratch.opcode = EEOP_QUAL;

	/* We can use ExprState's result as target for each qual expr. */
	scratch.result = esb.result;

	foreach(lc, qual)
	{
		Expr	   *node = (Expr *) lfirst(lc);

		/* first evaluate expression */
		ExecInitExprRec(&esb, node, esb.result);

		/* then emit EEOP_QUAL to detect if it's false (or null) */
		scratch.d.qualexpr.jumpdone = -1;
		ExprEvalPushStep(&esb, &scratch);
		adjust_jumps = lappend_int(adjust_jumps,
								   esb.steps_len - 1);
	}

	/* adjust jump targets */
	foreach(lc, adjust_jumps)
	{
		ExprEvalStep *as = nth_step(&esb, lfirst_int(lc));

		Assert(as->opcode == EEOP_QUAL);
		Assert(as->d.qualexpr.jumpdone == -1);
		as->d.qualexpr.jumpdone = esb.steps_len;
	}

	/*
	 * At the end, we don't need to do anything more.  The last qual expr must
	 * have yielded TRUE, and since its result is stored in the desired output
	 * location, we're done.
	 */
	scratch.opcode = EEOP_DONE_RETURN;
	scratch.result = esb.result;
	ExprEvalPushStep(&esb, &scratch);

	return ExecReadyExpr(&esb);
}

/*
 * ExecInitCheck: prepare a check constraint for execution by ExecCheck
 *
 * This is much like ExecInitQual/ExecQual, except that a null result from
 * the conjunction is treated as TRUE.  This behavior is appropriate for
 * evaluating CHECK constraints, since SQL specifies that NULL constraint
 * conditions are not failures.
 *
 * Note that like ExecInitQual, this expects input in implicit-AND format.
 * Users of ExecCheck that have expressions in normal explicit-AND format
 * can just apply ExecInitExpr to produce suitable input for ExecCheck.
 */
ExprState *
ExecInitCheck(List *qual, PlanState *parent)
{
	/* short-circuit (here and in ExecCheck) for empty restriction list */
	if (qual == NIL)
		return NULL;

	Assert(IsA(qual, List));

	/*
	 * Just convert the implicit-AND list to an explicit AND (if there's more
	 * than one entry), and compile normally.  Unlike ExecQual, we can't
	 * short-circuit on NULL results, so the regular AND behavior is needed.
	 */
	return ExecInitExpr(make_ands_explicit(qual), parent);
}

/*
 * Call ExecInitExpr() on a list of expressions, return a list of ExprStates.
 */
List *
ExecInitExprList(List *nodes, PlanState *parent)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, nodes)
	{
		Expr	   *e = lfirst(lc);

		result = lappend(result, ExecInitExpr(e, parent));
	}

	return result;
}

/*
 *		ExecBuildProjectionInfo
 *
 * Build a ProjectionInfo node for evaluating the given tlist in the given
 * econtext, and storing the result into the tuple slot.  (Caller must have
 * ensured that tuple slot has a descriptor matching the tlist!)
 *
 * inputDesc can be NULL, but if it is not, we check to see whether simple
 * Vars in the tlist match the descriptor.  It is important to provide
 * inputDesc for relation-scan plan nodes, as a cross check that the relation
 * hasn't been changed since the plan was made.  At higher levels of a plan,
 * there is no need to recheck.
 *
 * This is implemented by internally building an ExprState that performs the
 * whole projection in one go.
 *
 * Caution: before PG v10, the targetList was a list of ExprStates; now it
 * should be the planner-created targetlist, since we do the compilation here.
 */
ProjectionInfo *
ExecBuildProjectionInfo(List *targetList,
						ExprContext *econtext,
						TupleTableSlot *slot,
						PlanState *parent,
						TupleDesc inputDesc)
{
	ExprStateBuilder esb = {0};
	ExprEvalStep scratch = {0};
	ListCell   *lc;

	esb.result = ExprBuilderAllocNullableDatum(&esb);
	esb.expr = (Expr *) targetList;
	esb.parent = parent;

	/* Insert EEOP_*_FETCHSOME steps as needed */
	ExecInitExprSlots(&esb, (Node *) targetList);

	/* Now compile each tlist column */
	foreach(lc, targetList)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		Var		   *variable = NULL;
		AttrNumber	attnum = 0;
		bool		isSafeVar = false;

		/*
		 * If tlist expression is a safe non-system Var, use the fast-path
		 * ASSIGN_*_VAR opcodes.  "Safe" means that we don't need to apply
		 * CheckVarSlotCompatibility() during plan startup.  If a source slot
		 * was provided, we make the equivalent tests here; if a slot was not
		 * provided, we assume that no check is needed because we're dealing
		 * with a non-relation-scan-level expression.
		 */
		if (tle->expr != NULL &&
			IsA(tle->expr, Var) &&
			((Var *) tle->expr)->varattno > 0)
		{
			/* Non-system Var, but how safe is it? */
			variable = (Var *) tle->expr;
			attnum = variable->varattno;

			if (inputDesc == NULL)
				isSafeVar = true;	/* can't check, just assume OK */
			else if (attnum <= inputDesc->natts)
			{
				Form_pg_attribute attr = TupleDescAttr(inputDesc, attnum - 1);

				/*
				 * If user attribute is dropped or has a type mismatch, don't
				 * use ASSIGN_*_VAR.  Instead let the normal expression
				 * machinery handle it (which'll possibly error out).
				 */
				if (!attr->attisdropped && variable->vartype == attr->atttypid)
				{
					isSafeVar = true;
				}
			}
		}

		if (isSafeVar)
		{
			/* Fast-path: just generate an EEOP_ASSIGN_*_VAR step */
			switch (variable->varno)
			{
				case INNER_VAR:
					/* get the tuple from the inner node */
					scratch.opcode = EEOP_ASSIGN_INNER_VAR;
					break;

				case OUTER_VAR:
					/* get the tuple from the outer node */
					scratch.opcode = EEOP_ASSIGN_OUTER_VAR;
					break;

					/* INDEX_VAR is handled by default case */

				default:
					/* get the tuple from the relation being scanned */
					scratch.opcode = EEOP_ASSIGN_SCAN_VAR;
					break;
			}

			scratch.d.assign_var.attnum = attnum - 1;
			scratch.d.assign_var.resultnum = tle->resno - 1;
			ExprEvalPushStep(&esb, &scratch);
		}
		else
		{
			/*
			 * Otherwise, compile the column expression normally.
			 *
			 * We can't tell the expression to evaluate directly into the
			 * result slot, as the result slot (and the exprstate for that
			 * matter) can change between executions.  We instead evaluate
			 * into the ExprState's result and then move.
			 */
			ExecInitExprRec(&esb, tle->expr, esb.result);

			/*
			 * Column might be referenced multiple times in upper nodes, so
			 * force value to R/O - but only if it could be an expanded datum.
			 */
			if (get_typlen(exprType((Node *) tle->expr)) == -1)
				scratch.opcode = EEOP_ASSIGN_TMP_MAKE_RO;
			else
				scratch.opcode = EEOP_ASSIGN_TMP;
			scratch.result = esb.result;
			scratch.d.assign_tmp.resultnum = tle->resno - 1;
			ExprEvalPushStep(&esb, &scratch);
		}
	}

	scratch.opcode = EEOP_DONE_NO_RETURN;
	scratch.result.ptr.allocno = 0;
	ExprEvalPushStep(&esb, &scratch);

	{
		ProjectionInfo *projInfo;
		ExprState *state;

		/* We embed ExprState into ProjectionInfo instead of doing extra palloc */
		projInfo = ExecReadyExprEmbedded(&esb, offsetof(ProjectionInfo, pi_state));

		projInfo->type = T_ProjectionInfo;
		projInfo->pi_exprContext = econtext;

		state = &projInfo->pi_state;
		Assert(IsA(state, ExprState));

		state->resultslot = slot;

		return projInfo;
	}
}

/*
 * ExecPrepareExpr --- initialize for expression execution outside a normal
 * Plan tree context.
 *
 * This differs from ExecInitExpr in that we don't assume the caller is
 * already running in the EState's per-query context.  Also, we run the
 * passed expression tree through expression_planner() to prepare it for
 * execution.  (In ordinary Plan trees the regular planning process will have
 * made the appropriate transformations on expressions, but for standalone
 * expressions this won't have happened.)
 */
ExprState *
ExecPrepareExpr(Expr *node, EState *estate)
{
	ExprState  *result;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	node = expression_planner(node);

	result = ExecInitExpr(node, NULL);

	MemoryContextSwitchTo(oldcontext);

	return result;
}

/*
 * ExecPrepareQual --- initialize for qual execution outside a normal
 * Plan tree context.
 *
 * This differs from ExecInitQual in that we don't assume the caller is
 * already running in the EState's per-query context.  Also, we run the
 * passed expression tree through expression_planner() to prepare it for
 * execution.  (In ordinary Plan trees the regular planning process will have
 * made the appropriate transformations on expressions, but for standalone
 * expressions this won't have happened.)
 */
ExprState *
ExecPrepareQual(List *qual, EState *estate)
{
	ExprState  *result;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	qual = (List *) expression_planner((Expr *) qual);

	result = ExecInitQual(qual, NULL);

	MemoryContextSwitchTo(oldcontext);

	return result;
}

/*
 * ExecPrepareCheck -- initialize check constraint for execution outside a
 * normal Plan tree context.
 *
 * See ExecPrepareExpr() and ExecInitCheck() for details.
 */
ExprState *
ExecPrepareCheck(List *qual, EState *estate)
{
	ExprState  *result;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	qual = (List *) expression_planner((Expr *) qual);

	result = ExecInitCheck(qual, NULL);

	MemoryContextSwitchTo(oldcontext);

	return result;
}

/*
 * Call ExecPrepareExpr() on each member of a list of Exprs, and return
 * a list of ExprStates.
 *
 * See ExecPrepareExpr() for details.
 */
List *
ExecPrepareExprList(List *nodes, EState *estate)
{
	List	   *result = NIL;
	MemoryContext oldcontext;
	ListCell   *lc;

	/* Ensure that the list cell nodes are in the right context too */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	foreach(lc, nodes)
	{
		Expr	   *e = (Expr *) lfirst(lc);

		result = lappend(result, ExecPrepareExpr(e, estate));
	}

	MemoryContextSwitchTo(oldcontext);

	return result;
}

/*
 * ExecCheck - evaluate a check constraint
 *
 * For check constraints, a null result is taken as TRUE, ie the constraint
 * passes.
 *
 * The check constraint may have been prepared with ExecInitCheck
 * (possibly via ExecPrepareCheck) if the caller had it in implicit-AND
 * format, but a regular boolean expression prepared with ExecInitExpr or
 * ExecPrepareExpr works too.
 */
bool
ExecCheck(ExprState *state, ExprContext *econtext)
{
	Datum		ret;
	bool		isnull;

	/* short-circuit (here and in ExecInitCheck) for empty restriction list */
	if (state == NULL)
		return true;

	/* verify that expression was not compiled using ExecInitQual */
	Assert(!(state->flags & EEO_FLAG_IS_QUAL));

	ret = ExecEvalExprSwitchContext(state, econtext, &isnull);

	if (isnull)
		return true;

	return DatumGetBool(ret);
}

/*
 * Prepare a compiled expression for execution.  This has to be called for
 * every ExprState before it can be executed.
 *
 * NB: While this currently only calls ExecReadyInterpretedExpr(),
 * this will likely get extended to further expression evaluation methods.
 * Therefore this should be used instead of directly calling
 * ExecReadyInterpretedExpr().
 */
static void*
ExecReadyExprEmbedded(ExprStateBuilder *esb, size_t prefix)
{
	ExprState *state;
	size_t sz;
	char *p;

	Assert(esb->steps_len == list_length(esb->steps));

	sz =
		/* size of struct this is embedded in, needs to be trailing */
		prefix +
		/* space for expression steps */
		offsetof(ExprState, steps) * esb->steps_len * sizeof(ExprEvalStep) +
		/* space for mutable data */
		esb->allocation_size;

	p = palloc(sz);

	/* zero prefix */
	memset(p, 0, prefix);

	/* initialize state */
	state = (ExprState *) (p + prefix);

	memset(state, 0, sizeof(ExprState));

	state->tag = T_ExprState;
	state->expr = esb->expr;
	state->parent = esb->parent;
	state->flags = esb->flags;
	state->steps_final_len = esb->steps_len;
	state->mutable_off = offsetof(ExprState, steps) * esb->steps_len * sizeof(ExprEvalStep);

	/* copy in step data */
	{
		ListCell *lc;
		int off = 0;

		foreach(lc, esb->steps)
		{
			memcpy(&state->steps[off], lfirst(lc), sizeof(ExprEvalStep));
			off++;
		}
	}

	/* copy in allocations */
	{
		ListCell *lc;
		char *mut_data = (char *) state + state->mutable_off;
		Size mut_data_idx = 0;

		foreach(lc, esb->allocations)
		{
			ExprStateAllocation *alloc = (ExprStateAllocation *) lfirst(lc);

			if (alloc->initial_content != NULL)
			{
				memcpy(&mut_data[mut_data_idx], alloc->initial_content, alloc->sz);
			}
			else if (alloc->zeroed)
			{
				memset(&mut_data[mut_data_idx], 0, alloc->sz);
			}

			mut_data_idx += alloc->sz;
		}
	}

	if (jit_compile_expr(state, esb))
		return p;

	ExecReadyInterpretedExpr(state, esb);

	return p;
}

static ExprState*
ExecReadyExpr(ExprStateBuilder *esb)
{
	ExprState *state = ExecReadyExprEmbedded(esb, 0);

	Assert(IsA(state, ExprState));

	return state;
}

/*
 * Append the steps necessary for the evaluation of node to esb->steps,
 * possibly recursing into sub-expressions of node.
 *
 * node - expression to evaluate
 * esb - ExprStateBuilder to whose ->steps to append the necessary operations
 * result - where to store the result of the node into
 */
static void
ExecInitExprRec(ExprStateBuilder *esb, Expr *node, RelNullableDatum result)
{
	ExprEvalStep scratch = {0};

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	/* Step's output location is always what the caller gave us */
	//Assert(result != NULL);
	scratch.result = result;

	/* cases should be ordered as they are in enum NodeTag */
	switch (nodeTag(node))
	{
		case T_Var:
			{
				Var		   *variable = (Var *) node;

				if (variable->varattno == InvalidAttrNumber)
				{
					/* whole-row Var */
					ExecInitWholeRowVar(&scratch, variable, esb);
				}
				else if (variable->varattno <= 0)
				{
					/* system column */
					scratch.d.var.attnum = variable->varattno;
					scratch.d.var.vartype = variable->vartype;
					switch (variable->varno)
					{
						case INNER_VAR:
							scratch.opcode = EEOP_INNER_SYSVAR;
							break;
						case OUTER_VAR:
							scratch.opcode = EEOP_OUTER_SYSVAR;
							break;

							/* INDEX_VAR is handled by default case */

						default:
							scratch.opcode = EEOP_SCAN_SYSVAR;
							break;
					}
				}
				else
				{
					/* regular user column */
					scratch.d.var.attnum = variable->varattno - 1;
					scratch.d.var.vartype = variable->vartype;
					switch (variable->varno)
					{
						case INNER_VAR:
							scratch.opcode = EEOP_INNER_VAR;
							break;
						case OUTER_VAR:
							scratch.opcode = EEOP_OUTER_VAR;
							break;

							/* INDEX_VAR is handled by default case */

						default:
							scratch.opcode = EEOP_SCAN_VAR;
							break;
					}
				}

				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_Const:
			{
				Const	   *con = (Const *) node;

				scratch.opcode = EEOP_CONST;
				scratch.d.constval.value.value = con->constvalue;
				scratch.d.constval.value.isnull = con->constisnull;

				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_Param:
			{
				Param	   *param = (Param *) node;
				ParamListInfo params;

				switch (param->paramkind)
				{
					case PARAM_EXEC:
						scratch.opcode = EEOP_PARAM_EXEC;
						scratch.d.param.paramid = param->paramid;
						scratch.d.param.paramtype = param->paramtype;
						ExprEvalPushStep(esb, &scratch);
						break;
					case PARAM_EXTERN:

						/*
						 * If we have a relevant ParamCompileHook, use it;
						 * otherwise compile a standard EEOP_PARAM_EXTERN
						 * step.  ext_params, if supplied, takes precedence
						 * over info from the parent node's EState (if any).
						 */
						if (esb->ext_params)
							params = esb->ext_params;
						else if (esb->parent &&
								 esb->parent->state)
							params = esb->parent->state->es_param_list_info;
						else
							params = NULL;
						if (params && params->paramCompile)
						{
							scratch.opcode = EEOP_PARAM_CALLBACK;
							scratch.d.cparam.param = *param;
							scratch.d.cparam.paramarg = NULL;
							scratch.d.cparam.paramfunc =
								params->paramCompile(params, param, &scratch.d.cparam.paramarg);
							ExprEvalPushStep(esb, &scratch);
						}
						else
						{
							scratch.opcode = EEOP_PARAM_EXTERN;
							scratch.d.param.paramid = param->paramid;
							scratch.d.param.paramtype = param->paramtype;
							ExprEvalPushStep(esb, &scratch);
						}
						break;
					default:
						elog(ERROR, "unrecognized paramkind: %d",
							 (int) param->paramkind);
						break;
				}
				break;
			}

		case T_Aggref:
			{
				Aggref	   *aggref = (Aggref *) node;
				AggrefExprState *astate = makeNode(AggrefExprState);

				scratch.opcode = EEOP_AGGREF;
				scratch.d.aggref.astate = astate;
				astate->aggref = aggref;

				if (esb->parent && IsA(esb->parent, AggState))
				{
					AggState   *aggstate = (AggState *) esb->parent;

					aggstate->aggs = lappend(aggstate->aggs, astate);
					aggstate->numaggs++;
				}
				else
				{
					/* planner messed up */
					elog(ERROR, "Aggref found in non-Agg plan node");
				}

				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_GroupingFunc:
			{
				GroupingFunc *grp_node = (GroupingFunc *) node;
				Agg		   *agg;

				if (!esb->parent || !IsA(esb->parent, AggState) ||
					!IsA(esb->parent->plan, Agg))
					elog(ERROR, "GroupingFunc found in non-Agg plan node");

				scratch.opcode = EEOP_GROUPING_FUNC;

				agg = (Agg *) (esb->parent->plan);

				if (agg->groupingSets)
					scratch.d.grouping_func.clauses = grp_node->cols;
				else
					scratch.d.grouping_func.clauses = NIL;

				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_WindowFunc:
			{
				WindowFunc *wfunc = (WindowFunc *) node;
				WindowFuncExprState *wfstate = makeNode(WindowFuncExprState);

				wfstate->wfunc = wfunc;

				if (esb->parent && IsA(esb->parent, WindowAggState))
				{
					WindowAggState *winstate = (WindowAggState *) esb->parent;
					int			nfuncs;

					winstate->funcs = lappend(winstate->funcs, wfstate);
					nfuncs = ++winstate->numfuncs;
					if (wfunc->winagg)
						winstate->numaggs++;

					/* for now initialize agg using old style expressions */
					wfstate->args = ExecInitExprList(wfunc->args,
													 esb->parent);
					wfstate->aggfilter = ExecInitExpr(wfunc->aggfilter,
													  esb->parent);

					/*
					 * Complain if the windowfunc's arguments contain any
					 * windowfuncs; nested window functions are semantically
					 * nonsensical.  (This should have been caught earlier,
					 * but we defend against it here anyway.)
					 */
					if (nfuncs != winstate->numfuncs)
						ereport(ERROR,
								(errcode(ERRCODE_WINDOWING_ERROR),
								 errmsg("window function calls cannot be nested")));
				}
				else
				{
					/* planner messed up */
					elog(ERROR, "WindowFunc found in non-WindowAgg plan node");
				}

				scratch.opcode = EEOP_WINDOW_FUNC;
				scratch.d.window_func.wfstate = wfstate;
				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_SubscriptingRef:
			{
				SubscriptingRef *sbsref = (SubscriptingRef *) node;

				ExecInitSubscriptingRef(&scratch, sbsref, esb, result);
				break;
			}

		case T_FuncExpr:
			{
				FuncExpr   *func = (FuncExpr *) node;

				ExecInitFunc(&scratch, node,
							 func->args, func->funcid, func->inputcollid,
							 esb);
				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_OpExpr:
			{
				OpExpr	   *op = (OpExpr *) node;

				ExecInitFunc(&scratch, node,
							 op->args, op->opfuncid, op->inputcollid,
							 esb);
				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_DistinctExpr:
			{
				DistinctExpr *op = (DistinctExpr *) node;

				ExecInitFunc(&scratch, node,
							 op->args, op->opfuncid, op->inputcollid,
							 esb);

				/*
				 * Change opcode of call instruction to EEOP_DISTINCT.
				 *
				 * XXX: historically we've not called the function usage
				 * pgstat infrastructure - that seems inconsistent given that
				 * we do so for normal function *and* operator evaluation.  If
				 * we decided to do that here, we'd probably want separate
				 * opcodes for FUSAGE or not.
				 */
				scratch.opcode = EEOP_DISTINCT;
				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_NullIfExpr:
			{
				NullIfExpr *op = (NullIfExpr *) node;

				ExecInitFunc(&scratch, node,
							 op->args, op->opfuncid, op->inputcollid,
							 esb);

				/*
				 * Change opcode of call instruction to EEOP_NULLIF.
				 *
				 * XXX: historically we've not called the function usage
				 * pgstat infrastructure - that seems inconsistent given that
				 * we do so for normal function *and* operator evaluation.  If
				 * we decided to do that here, we'd probably want separate
				 * opcodes for FUSAGE or not.
				 */
				scratch.opcode = EEOP_NULLIF;
				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *opexpr = (ScalarArrayOpExpr *) node;
				Expr	   *scalararg;
				Expr	   *arrayarg;
				FmgrInfo   *finfo;
				RelFunctionCallInfo fcinfo_rel;
				FunctionCallInfo fcinfo;
				AclResult	aclresult;

				Assert(list_length(opexpr->args) == 2);
				scalararg = (Expr *) linitial(opexpr->args);
				arrayarg = (Expr *) lsecond(opexpr->args);

				/* Check permission to call function */
				aclresult = pg_proc_aclcheck(opexpr->opfuncid,
											 GetUserId(),
											 ACL_EXECUTE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, OBJECT_FUNCTION,
								   get_func_name(opexpr->opfuncid));
				InvokeFunctionExecuteHook(opexpr->opfuncid);

				/* Set up the primary fmgr lookup information */
				finfo = palloc0(sizeof(FmgrInfo));
				fcinfo_rel = ExprBuilderAllocFunctionCallInfo(esb, 2, &fcinfo);

				fmgr_info(opexpr->opfuncid, finfo);
				fmgr_info_set_expr((Node *) node, finfo);
				InitFunctionCallInfoData(*fcinfo, finfo, 2,
										 opexpr->inputcollid, NULL, NULL);

				/* Evaluate scalar directly into left function argument */
				ExecInitExprRec(esb, scalararg, RelFCIIdx(fcinfo_rel, 0));

				/*
				 * Evaluate array argument into our return value.  There's no
				 * danger in that, because the return value is guaranteed to
				 * be overwritten by EEOP_SCALARARRAYOP, and will not be
				 * passed to any other expression.
				 */
				ExecInitExprRec(esb, arrayarg, result);

				/* And perform the operation */
				scratch.opcode = EEOP_SCALARARRAYOP;
				scratch.d.scalararrayop.element_type = InvalidOid;
				scratch.d.scalararrayop.useOr = opexpr->useOr;
				scratch.d.scalararrayop.fn_strict = finfo->fn_strict;
				scratch.d.scalararrayop.fcinfo_data = fcinfo_rel;
				scratch.d.scalararrayop.fn_addr = finfo->fn_addr;
				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_BoolExpr:
			{
				BoolExpr   *boolexpr = (BoolExpr *) node;
				int			nargs = list_length(boolexpr->args);
				List	   *adjust_jumps = NIL;
				int			off;
				ListCell   *lc;

				/* allocate scratch memory used by all steps of AND/OR */
				if (boolexpr->boolop != NOT_EXPR)
					scratch.d.boolexpr.anynull = ExprBuilderAllocBool(esb);

				/*
				 * For each argument evaluate the argument itself, then
				 * perform the bool operation's appropriate handling.
				 *
				 * We can evaluate each argument into our result area, since
				 * the short-circuiting logic means we only need to remember
				 * previous NULL values.
				 *
				 * AND/OR is split into separate STEP_FIRST (one) / STEP (zero
				 * or more) / STEP_LAST (one) steps, as each of those has to
				 * perform different work.  The FIRST/LAST split is valid
				 * because AND/OR have at least two arguments.
				 */
				off = 0;
				foreach(lc, boolexpr->args)
				{
					Expr	   *arg = (Expr *) lfirst(lc);

					/* Evaluate argument into our output variable */
					ExecInitExprRec(esb, arg, result);

					/* Perform the appropriate step type */
					switch (boolexpr->boolop)
					{
						case AND_EXPR:
							Assert(nargs >= 2);

							if (off == 0)
								scratch.opcode = EEOP_BOOL_AND_STEP_FIRST;
							else if (off + 1 == nargs)
								scratch.opcode = EEOP_BOOL_AND_STEP_LAST;
							else
								scratch.opcode = EEOP_BOOL_AND_STEP;
							break;
						case OR_EXPR:
							Assert(nargs >= 2);

							if (off == 0)
								scratch.opcode = EEOP_BOOL_OR_STEP_FIRST;
							else if (off + 1 == nargs)
								scratch.opcode = EEOP_BOOL_OR_STEP_LAST;
							else
								scratch.opcode = EEOP_BOOL_OR_STEP;
							break;
						case NOT_EXPR:
							Assert(nargs == 1);

							scratch.opcode = EEOP_BOOL_NOT_STEP;
							break;
						default:
							elog(ERROR, "unrecognized boolop: %d",
								 (int) boolexpr->boolop);
							break;
					}

					scratch.d.boolexpr.jumpdone = -1;
					ExprEvalPushStep(esb, &scratch);
					adjust_jumps = lappend_int(adjust_jumps,
											   esb->steps_len - 1);
					off++;
				}

				/* adjust jump targets */
				foreach(lc, adjust_jumps)
				{
					ExprEvalStep *as = nth_step(esb, lfirst_int(lc));

					Assert(as->d.boolexpr.jumpdone == -1);
					as->d.boolexpr.jumpdone = esb->steps_len;
				}

				break;
			}

		case T_SubPlan:
			{
				SubPlan    *subplan = (SubPlan *) node;
				SubPlanState *sstate;

				if (!esb->parent)
					elog(ERROR, "SubPlan found with no parent plan");

				sstate = ExecInitSubPlan(subplan, esb->parent);

				/* add SubPlanState nodes to esb->parent->subPlan */
				esb->parent->subPlan = lappend(esb->parent->subPlan,
												 sstate);

				scratch.opcode = EEOP_SUBPLAN;
				scratch.d.subplan.sstate = sstate;

				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_AlternativeSubPlan:
			{
				AlternativeSubPlan *asplan = (AlternativeSubPlan *) node;
				AlternativeSubPlanState *asstate;

				if (!esb->parent)
					elog(ERROR, "AlternativeSubPlan found with no parent plan");

				asstate = ExecInitAlternativeSubPlan(asplan, esb->parent);

				scratch.opcode = EEOP_ALTERNATIVE_SUBPLAN;
				scratch.d.alternative_subplan.asstate = asstate;

				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_FieldSelect:
			{
				FieldSelect *fselect = (FieldSelect *) node;

				/* evaluate row/record argument into result area */
				ExecInitExprRec(esb, fselect->arg, result);

				/* and extract field */
				scratch.opcode = EEOP_FIELDSELECT;
				scratch.d.fieldselect.fieldnum = fselect->fieldnum;
				scratch.d.fieldselect.resulttype = fselect->resulttype;
				scratch.d.fieldselect.argdesc = NULL;

				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_FieldStore:
			{
				FieldStore *fstore = (FieldStore *) node;
				TupleDesc	tupDesc;
				TupleDesc  *descp;
				RelNullableDatumArray columns;
				int			ncolumns;
				ListCell   *l1,
						   *l2;

				/* find out the number of columns in the composite type */
				tupDesc = lookup_rowtype_tupdesc(fstore->resulttype, -1);
				ncolumns = tupDesc->natts;
				DecrTupleDescRefCount(tupDesc);

				/* create workspace for column values */
				columns = ExprBuilderAllocNullableDatumArray(esb, ncolumns);

				/* create workspace for runtime tupdesc cache */
				descp = (TupleDesc *) palloc(sizeof(TupleDesc));
				*descp = NULL;

				/* emit code to evaluate the composite input value */
				ExecInitExprRec(esb, fstore->arg, result);

				/* next, deform the input tuple into our workspace */
				scratch.opcode = EEOP_FIELDSTORE_DEFORM;
				scratch.d.fieldstore.fstore = fstore;
				scratch.d.fieldstore.argdesc = descp;
				scratch.d.fieldstore.columns = columns;
				scratch.d.fieldstore.ncolumns = ncolumns;
				ExprEvalPushStep(esb, &scratch);

				/* evaluate new field values, store in workspace columns */
				forboth(l1, fstore->newvals, l2, fstore->fieldnums)
				{
					Expr	   *e = (Expr *) lfirst(l1);
					AttrNumber	fieldnum = lfirst_int(l2);
					RelNullableDatum save_innermost_caseval;

					if (fieldnum <= 0 || fieldnum > ncolumns)
						elog(ERROR, "field number %d is out of range in FieldStore",
							 fieldnum);

					/*
					 * Use the CaseTestExpr mechanism to pass down the old
					 * value of the field being replaced; this is needed in
					 * case the newval is itself a FieldStore or
					 * SubscriptingRef that has to obtain and modify the old
					 * value.  It's safe to reuse the CASE mechanism because
					 * there cannot be a CASE between here and where the value
					 * would be needed, and a field assignment can't be within
					 * a CASE either.  (So saving and restoring
					 * innermost_caseval is just paranoia, but let's do it
					 * anyway.)
					 *
					 * Another non-obvious point is that it's safe to use the
					 * field's values[]/nulls[] entries as both the caseval
					 * source and the result address for this subexpression.
					 * That's okay only because (1) both FieldStore and
					 * SubscriptingRef evaluate their arg or refexpr inputs
					 * first, and (2) any such CaseTestExpr is directly the
					 * arg or refexpr input.  So any read of the caseval will
					 * occur before there's a chance to overwrite it.  Also,
					 * if multiple entries in the newvals/fieldnums lists
					 * target the same field, they'll effectively be applied
					 * left-to-right which is what we want.
					 */
					save_innermost_caseval = esb->innermost_caseval;
					esb->innermost_caseval = RelArrayIdx(columns, fieldnum - 1);

					ExecInitExprRec(esb, e, esb->innermost_caseval);

					esb->innermost_caseval = save_innermost_caseval;
				}

				/* finally, form result tuple */
				scratch.opcode = EEOP_FIELDSTORE_FORM;
				scratch.d.fieldstore.fstore = fstore;
				scratch.d.fieldstore.argdesc = descp;
				scratch.d.fieldstore.columns = columns;
				scratch.d.fieldstore.ncolumns = ncolumns;
				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_RelabelType:
			{
				/* relabel doesn't need to do anything at runtime */
				RelabelType *relabel = (RelabelType *) node;

				ExecInitExprRec(esb, relabel->arg, result);
				break;
			}

		case T_CoerceViaIO:
			{
				CoerceViaIO *iocoerce = (CoerceViaIO *) node;
				Oid			iofunc;
				bool		typisvarlena;
				Oid			typioparam;
				FunctionCallInfo fcinfo_in;
				FunctionCallInfo fcinfo_out;
				FmgrInfo   *finfo_in = palloc0(sizeof(FmgrInfo));
				FmgrInfo   *finfo_out = palloc0(sizeof(FmgrInfo));

				/* evaluate argument into step's result area */
				ExecInitExprRec(esb, iocoerce->arg, result);

				/*
				 * Prepare both output and input function calls, to be
				 * evaluated inside a single evaluation step for speed - this
				 * can be a very common operation.
				 *
				 * We don't check permissions here as a type's input/output
				 * function are assumed to be executable by everyone.
				 */
				scratch.opcode = EEOP_IOCOERCE;

				/* lookup the source type's output function */
				scratch.d.iocoerce.fcinfo_data_out =
					ExprBuilderAllocFunctionCallInfo(esb, 1, &fcinfo_out);

				getTypeOutputInfo(exprType((Node *) iocoerce->arg),
								  &iofunc, &typisvarlena);
				fmgr_info(iofunc, finfo_out);
				fmgr_info_set_expr((Node *) node, finfo_out);
				InitFunctionCallInfoData(*fcinfo_out, finfo_out, 1,
										 InvalidOid, NULL, NULL);
				scratch.d.iocoerce.fn_addr_out = finfo_out->fn_addr;

				/* lookup the result type's input function */
				scratch.d.iocoerce.fcinfo_data_in =
					ExprBuilderAllocFunctionCallInfo(esb, 3, &fcinfo_in);

				getTypeInputInfo(iocoerce->resulttype,
								 &iofunc, &typioparam);
				fmgr_info(iofunc, finfo_in);
				fmgr_info_set_expr((Node *) node, finfo_in);
				InitFunctionCallInfoData(*fcinfo_in, finfo_in, 3,
										 InvalidOid, NULL, NULL);
				scratch.d.iocoerce.fn_strict_in = finfo_in->fn_strict;
				scratch.d.iocoerce.fn_addr_in = finfo_in->fn_addr;

				/*
				 * We can preload the second and third arguments for the input
				 * function, since they're constants.
				 */
				fcinfo_in->args[1].value = ObjectIdGetDatum(typioparam);
				fcinfo_in->args[1].isnull = false;
				fcinfo_in->args[2].value = Int32GetDatum(-1);
				fcinfo_in->args[2].isnull = false;

				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_ArrayCoerceExpr:
			{
				ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) node;
				Oid			resultelemtype;

				/* evaluate argument into step's result area */
				ExecInitExprRec(esb, acoerce->arg, result);

				resultelemtype = get_element_type(acoerce->resulttype);
				if (!OidIsValid(resultelemtype))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("target type is not an array")));

				if (IsA(acoerce->elemexpr, CaseTestExpr))
				{
					/*
					 * If the transform expression is just the element itself,
					 * the elements are binary compatible, and just the header
					 * needs to be adjusted. In that case there's no need for
					 * per-element transformations.
					 */
					scratch.opcode = EEOP_ARRAYCOERCE_RELABEL;
					scratch.d.arraycoerce.amstate = NULL;
					scratch.d.arraycoerce.resultelemtype = resultelemtype;
					scratch.d.arraycoerce.jumpnext = -1;
					ExprEvalPushStep(esb, &scratch);
				}
				else
				{
					int			startstep;
					RelNullableDatum save_innermost_caseval;
					ArrayMapState *amstate;

					/* Set up workspace for array_map */
					amstate = (ArrayMapState *) palloc0(sizeof(ArrayMapState));

					/* build a step to unpack the array, stages element in result */
					scratch.opcode = EEOP_ARRAYCOERCE_UNPACK;
					scratch.d.arraycoerce.amstate = amstate;
					scratch.d.arraycoerce.resultelemtype = resultelemtype;
					scratch.d.arraycoerce.jumpnext = -1;
					ExprEvalPushStep(esb, &scratch);
					startstep = esb->steps_len - 1;

					/* evaluate the per-element expression, from result into result */
					// XXX: separate allocation instead, for robustness?
					save_innermost_caseval = esb->innermost_caseval;
					esb->innermost_caseval = result;
					ExecInitExprRec(esb, acoerce->elemexpr, result);
					esb->innermost_caseval = save_innermost_caseval;

					/* jump backwards after staging element in result, or form final array */
					scratch.opcode = EEOP_ARRAYCOERCE_PACK;
					scratch.d.arraycoerce.amstate = amstate;
					scratch.d.arraycoerce.resultelemtype = resultelemtype;
					scratch.d.arraycoerce.jumpnext = startstep + 1;
					ExprEvalPushStep(esb, &scratch);

					Assert(nth_step(esb, startstep)->opcode == EEOP_ARRAYCOERCE_UNPACK);
					nth_step(esb, startstep)->d.arraycoerce.jumpnext = esb->steps_len;
				}
				break;
			}

		case T_ConvertRowtypeExpr:
			{
				ConvertRowtypeExpr *convert = (ConvertRowtypeExpr *) node;

				/* evaluate argument into step's result area */
				ExecInitExprRec(esb, convert->arg, result);

				/* and push conversion step */
				scratch.opcode = EEOP_CONVERT_ROWTYPE;
				scratch.d.convert_rowtype.convert = convert;
				scratch.d.convert_rowtype.indesc = NULL;
				scratch.d.convert_rowtype.outdesc = NULL;
				scratch.d.convert_rowtype.map = NULL;
				scratch.d.convert_rowtype.initialized = false;

				ExprEvalPushStep(esb, &scratch);
				break;
			}

			/* note that CaseWhen expressions are handled within this block */
		case T_CaseExpr:
			{
				CaseExpr   *caseExpr = (CaseExpr *) node;
				List	   *adjust_jumps = NIL;
				RelNullableDatum caseval;
				ListCell   *lc;

				/*
				 * If there's a test expression, we have to evaluate it and
				 * save the value where the CaseTestExpr placeholders can find
				 * it.
				 */
				if (caseExpr->arg != NULL)
				{
					/* Evaluate testexpr into caseval/casenull workspace */
					caseval = ExprBuilderAllocNullableDatum(esb);

					ExecInitExprRec(esb, caseExpr->arg, caseval);

					/*
					 * Since value might be read multiple times, force to R/O
					 * - but only if it could be an expanded datum.
					 */
					if (get_typlen(exprType((Node *) caseExpr->arg)) == -1)
					{
						/* change caseval in-place */
						scratch.opcode = EEOP_MAKE_READONLY;
						scratch.result = caseval;
						scratch.d.make_readonly.value = caseval;
						ExprEvalPushStep(esb, &scratch);
						/* restore normal settings of scratch fields */
						scratch.result = result;
					}
				}

				/*
				 * Prepare to evaluate each of the WHEN clauses in turn; as
				 * soon as one is true we return the value of the
				 * corresponding THEN clause.  If none are true then we return
				 * the value of the ELSE clause, or NULL if there is none.
				 */
				foreach(lc, caseExpr->args)
				{
					CaseWhen   *when = (CaseWhen *) lfirst(lc);
					RelNullableDatum save_innermost_caseval;
					int			whenstep;

					/*
					 * Make testexpr result available to CaseTestExpr nodes
					 * within the condition.  We must save and restore prior
					 * setting of innermost_caseval fields, in case this node
					 * is itself within a larger CASE.
					 *
					 * If there's no test expression, we don't actually need
					 * to save and restore these fields; but it's less code to
					 * just do so unconditionally.
					 */
					save_innermost_caseval = esb->innermost_caseval;
					esb->innermost_caseval = caseval;

					/* evaluate condition into CASE's result variables */
					ExecInitExprRec(esb, when->expr, result);

					esb->innermost_caseval = save_innermost_caseval;

					/* If WHEN result isn't true, jump to next CASE arm */
					scratch.opcode = EEOP_JUMP_IF_NOT_TRUE;
					scratch.d.jump.jumpdone = -1;	/* computed later */
					ExprEvalPushStep(esb, &scratch);
					whenstep = esb->steps_len - 1;

					/*
					 * If WHEN result is true, evaluate THEN result, storing
					 * it into the CASE's result variables.
					 */
					ExecInitExprRec(esb, when->result, result);

					/* Emit JUMP step to jump to end of CASE's code */
					scratch.opcode = EEOP_JUMP;
					scratch.d.jump.jumpdone = -1;	/* computed later */
					ExprEvalPushStep(esb, &scratch);

					/*
					 * Don't know address for that jump yet, compute once the
					 * whole CASE expression is built.
					 */
					adjust_jumps = lappend_int(adjust_jumps,
											   esb->steps_len - 1);

					/*
					 * But we can set WHEN test's jump target now, to make it
					 * jump to the next WHEN subexpression or the ELSE.
					 */
					nth_step(esb, whenstep)->d.jump.jumpdone = esb->steps_len;
				}

				/* transformCaseExpr always adds a default */
				Assert(caseExpr->defresult);

				/* evaluate ELSE expr into CASE's result variables */
				ExecInitExprRec(esb, caseExpr->defresult, result);

				/* adjust jump targets */
				foreach(lc, adjust_jumps)
				{
					ExprEvalStep *as = nth_step(esb, lfirst_int(lc));

					Assert(as->opcode == EEOP_JUMP);
					Assert(as->d.jump.jumpdone == -1);
					as->d.jump.jumpdone = esb->steps_len;
				}

				break;
			}

		case T_CaseTestExpr:
			{
				/*
				 * Read from location identified by innermost_caseval.  Note
				 * that innermost_caseval could be NULL, if this node isn't
				 * actually within a CaseExpr, ArrayCoerceExpr, etc structure.
				 * That can happen because some parts of the system abuse
				 * CaseTestExpr to cause a read of a value externally supplied
				 * in econtext->caseValue_datum.  We'll take care of that
				 * scenario at runtime.
				 */
				scratch.opcode = EEOP_CASE_TESTVAL;
				scratch.d.casetest.value = esb->innermost_caseval;

				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_ArrayExpr:
			{
				ArrayExpr  *arrayexpr = (ArrayExpr *) node;
				int			nelems = list_length(arrayexpr->elements);
				ListCell   *lc;
				int			elemoff;

				/*
				 * Evaluate by computing each element, and then forming the
				 * array.  Elements are computed into scratch arrays
				 * associated with the ARRAYEXPR step.
				 */
				scratch.opcode = EEOP_ARRAYEXPR;
				scratch.d.arrayexpr.elements =
					ExprBuilderAllocNullableDatumArray(esb, nelems);
				scratch.d.arrayexpr.nelems = nelems;

				/* fill remaining fields of step */
				scratch.d.arrayexpr.multidims = arrayexpr->multidims;
				scratch.d.arrayexpr.elemtype = arrayexpr->element_typeid;

				/* do one-time catalog lookup for type info */
				get_typlenbyvalalign(arrayexpr->element_typeid,
									 &scratch.d.arrayexpr.elemlength,
									 &scratch.d.arrayexpr.elembyval,
									 &scratch.d.arrayexpr.elemalign);

				/* prepare to evaluate all arguments */
				elemoff = 0;
				foreach(lc, arrayexpr->elements)
				{
					Expr	   *e = (Expr *) lfirst(lc);

					ExecInitExprRec(esb, e,
									RelArrayIdx(scratch.d.arrayexpr.elements,
												elemoff));
					elemoff++;
				}

				/* and then collect all into an array */
				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_RowExpr:
			{
				RowExpr    *rowexpr = (RowExpr *) node;
				int			nelems = list_length(rowexpr->args);
				NullableDatum *elements;
				TupleDesc	tupdesc;
				int			i;
				ListCell   *l;

				/* Build tupdesc to describe result tuples */
				if (rowexpr->row_typeid == RECORDOID)
				{
					/* generic record, use types of given expressions */
					tupdesc = ExecTypeFromExprList(rowexpr->args);
				}
				else
				{
					/* it's been cast to a named type, use that */
					tupdesc = lookup_rowtype_tupdesc_copy(rowexpr->row_typeid, -1);
				}
				/* In either case, adopt RowExpr's column aliases */
				ExecTypeSetColNames(tupdesc, rowexpr->colnames);
				/* Bless the tupdesc in case it's now of type RECORD */
				BlessTupleDesc(tupdesc);

				/*
				 * In the named-type case, the tupdesc could have more columns
				 * than are in the args list, since the type might have had
				 * columns added since the ROW() was parsed.  We want those
				 * extra columns to go to nulls, so we make sure that the
				 * workspace arrays are large enough and then initialize any
				 * extra columns to read as NULLs.
				 */
				Assert(nelems <= tupdesc->natts);
				nelems = Max(nelems, tupdesc->natts);

				/*
				 * Evaluate by first building datums for each field, and then
				 * a final step forming the composite datum.
				 */
				scratch.opcode = EEOP_ROW;
				scratch.d.row.tupdesc = tupdesc;

				/* space for the individual field datums */
				scratch.d.row.elements =
					ExprBuilderAllocNullableDatumArrayInit(esb, nelems, &elements);
				/* as explained above, make sure any extra columns are null */
				for (int i = 0; i < nelems; i++)
					elements[i] = NULL_DATUM;

				/* Set up evaluation, skipping any deleted columns */
				i = 0;
				foreach(l, rowexpr->args)
				{
					Form_pg_attribute att = TupleDescAttr(tupdesc, i);
					Expr	   *e = (Expr *) lfirst(l);

					if (!att->attisdropped)
					{
						/*
						 * Guard against ALTER COLUMN TYPE on rowtype since
						 * the RowExpr was created.  XXX should we check
						 * typmod too?	Not sure we can be sure it'll be the
						 * same.
						 */
						if (exprType((Node *) e) != att->atttypid)
							ereport(ERROR,
									(errcode(ERRCODE_DATATYPE_MISMATCH),
									 errmsg("ROW() column has type %s instead of type %s",
											format_type_be(exprType((Node *) e)),
											format_type_be(att->atttypid))));
					}
					else
					{
						/*
						 * Ignore original expression and insert a NULL. We
						 * don't really care what type of NULL it is, so
						 * always make an int4 NULL.
						 */
						e = (Expr *) makeNullConst(INT4OID, -1, InvalidOid);
					}

					/* Evaluate column expr into appropriate workspace slot */
					ExecInitExprRec(esb, e,
									RelArrayIdx(scratch.d.row.elements, i));
					elements[i].isnull = false;
					i++;
				}

				/* And finally build the row value */
				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_RowCompareExpr:
			{
				RowCompareExpr *rcexpr = (RowCompareExpr *) node;
				int			nopers = list_length(rcexpr->opnos);
				List	   *adjust_jumps = NIL;
				ListCell   *l_left_expr,
						   *l_right_expr,
						   *l_opno,
						   *l_opfamily,
						   *l_inputcollid;
				ListCell   *lc;

				/*
				 * Iterate over each field, prepare comparisons.  To handle
				 * NULL results, prepare jumps to after the expression.  If a
				 * comparison yields a != 0 result, jump to the final step.
				 */
				Assert(list_length(rcexpr->largs) == nopers);
				Assert(list_length(rcexpr->rargs) == nopers);
				Assert(list_length(rcexpr->opfamilies) == nopers);
				Assert(list_length(rcexpr->inputcollids) == nopers);

				forfive(l_left_expr, rcexpr->largs,
						l_right_expr, rcexpr->rargs,
						l_opno, rcexpr->opnos,
						l_opfamily, rcexpr->opfamilies,
						l_inputcollid, rcexpr->inputcollids)
				{
					Expr	   *left_expr = (Expr *) lfirst(l_left_expr);
					Expr	   *right_expr = (Expr *) lfirst(l_right_expr);
					Oid			opno = lfirst_oid(l_opno);
					Oid			opfamily = lfirst_oid(l_opfamily);
					Oid			inputcollid = lfirst_oid(l_inputcollid);
					int			strategy;
					Oid			lefttype;
					Oid			righttype;
					Oid			proc;
					FmgrInfo   *finfo;
					RelFunctionCallInfo fcinfo_rel;
					FunctionCallInfo fcinfo;

					get_op_opfamily_properties(opno, opfamily, false,
											   &strategy,
											   &lefttype,
											   &righttype);
					proc = get_opfamily_proc(opfamily,
											 lefttype,
											 righttype,
											 BTORDER_PROC);
					if (!OidIsValid(proc))
						elog(ERROR, "missing support function %d(%u,%u) in opfamily %u",
							 BTORDER_PROC, lefttype, righttype, opfamily);

					/* Set up the primary fmgr lookup information */
					finfo = palloc0(sizeof(FmgrInfo));
					fcinfo_rel =
						ExprBuilderAllocFunctionCallInfo(esb, 3, &fcinfo);

					fmgr_info(proc, finfo);
					fmgr_info_set_expr((Node *) node, finfo);
					InitFunctionCallInfoData(*fcinfo, finfo, 2,
											 inputcollid, NULL, NULL);

					/*
					 * If we enforced permissions checks on index support
					 * functions, we'd need to make a check here.  But the
					 * index support machinery doesn't do that, and thus
					 * neither does this code.
					 */

					/* evaluate left and right args directly into fcinfo */
					ExecInitExprRec(esb, left_expr, RelFCIIdx(fcinfo_rel, 0));
					ExecInitExprRec(esb, right_expr, RelFCIIdx(fcinfo_rel, 1));

					scratch.opcode = EEOP_ROWCOMPARE_STEP;
					scratch.d.rowcompare_step.fn_strict = finfo->fn_strict;
					scratch.d.rowcompare_step.fcinfo_data = fcinfo_rel;
					scratch.d.rowcompare_step.fn_addr = finfo->fn_addr;
					/* jump targets filled below */
					scratch.d.rowcompare_step.jumpnull = -1;
					scratch.d.rowcompare_step.jumpdone = -1;

					ExprEvalPushStep(esb, &scratch);
					adjust_jumps = lappend_int(adjust_jumps,
											   esb->steps_len - 1);
				}

				/*
				 * We could have a zero-column rowtype, in which case the rows
				 * necessarily compare equal.
				 */
				if (nopers == 0)
				{
					scratch.opcode = EEOP_CONST;
					scratch.d.constval.value.value = Int32GetDatum(0);
					scratch.d.constval.value.isnull = false;
					ExprEvalPushStep(esb, &scratch);
				}

				/* Finally, examine the last comparison result */
				scratch.opcode = EEOP_ROWCOMPARE_FINAL;
				scratch.d.rowcompare_final.rctype = rcexpr->rctype;
				ExprEvalPushStep(esb, &scratch);

				/* adjust jump targets */
				foreach(lc, adjust_jumps)
				{
					ExprEvalStep *as = nth_step(esb, lfirst_int(lc));

					Assert(as->opcode == EEOP_ROWCOMPARE_STEP);
					Assert(as->d.rowcompare_step.jumpdone == -1);
					Assert(as->d.rowcompare_step.jumpnull == -1);

					/* jump to comparison evaluation */
					as->d.rowcompare_step.jumpdone = esb->steps_len - 1;
					/* jump to the following expression */
					as->d.rowcompare_step.jumpnull = esb->steps_len;
				}

				break;
			}

		case T_CoalesceExpr:
			{
				CoalesceExpr *coalesce = (CoalesceExpr *) node;
				List	   *adjust_jumps = NIL;
				ListCell   *lc;

				/* We assume there's at least one arg */
				Assert(coalesce->args != NIL);

				/*
				 * Prepare evaluation of all coalesced arguments, after each
				 * one push a step that short-circuits if not null.
				 */
				foreach(lc, coalesce->args)
				{
					Expr	   *e = (Expr *) lfirst(lc);

					/* evaluate argument, directly into result datum */
					ExecInitExprRec(esb, e, result);

					/* if it's not null, skip to end of COALESCE expr */
					scratch.opcode = EEOP_JUMP_IF_NOT_NULL;
					scratch.d.jump.jumpdone = -1;	/* adjust later */
					ExprEvalPushStep(esb, &scratch);

					adjust_jumps = lappend_int(adjust_jumps,
											   esb->steps_len - 1);
				}

				/*
				 * No need to add a constant NULL return - we only can get to
				 * the end of the expression if a NULL already is being
				 * returned.
				 */

				/* adjust jump targets */
				foreach(lc, adjust_jumps)
				{
					ExprEvalStep *as = nth_step(esb, lfirst_int(lc));

					Assert(as->opcode == EEOP_JUMP_IF_NOT_NULL);
					Assert(as->d.jump.jumpdone == -1);
					as->d.jump.jumpdone = esb->steps_len;
				}

				break;
			}

		case T_MinMaxExpr:
			{
				MinMaxExpr *minmaxexpr = (MinMaxExpr *) node;
				int			nelems = list_length(minmaxexpr->args);
				TypeCacheEntry *typentry;
				FmgrInfo   *finfo;
				RelFunctionCallInfo fcinfo_rel;
				FunctionCallInfo fcinfo;
				ListCell   *lc;
				int			off;

				/* Look up the btree comparison function for the datatype */
				typentry = lookup_type_cache(minmaxexpr->minmaxtype,
											 TYPECACHE_CMP_PROC);
				if (!OidIsValid(typentry->cmp_proc))
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_FUNCTION),
							 errmsg("could not identify a comparison function for type %s",
									format_type_be(minmaxexpr->minmaxtype))));

				/*
				 * If we enforced permissions checks on index support
				 * functions, we'd need to make a check here.  But the index
				 * support machinery doesn't do that, and thus neither does
				 * this code.
				 */

				/* Perform function lookup */
				finfo = palloc0(sizeof(FmgrInfo));
				fcinfo_rel =
					ExprBuilderAllocFunctionCallInfo(esb, 2, &fcinfo);
				fmgr_info(typentry->cmp_proc, finfo);
				fmgr_info_set_expr((Node *) node, finfo);
				InitFunctionCallInfoData(*fcinfo, finfo, 2,
										 minmaxexpr->inputcollid, NULL, NULL);

				scratch.opcode = EEOP_MINMAX;
				/* allocate space to store arguments */
				scratch.d.minmax.arguments =
					ExprBuilderAllocNullableDatumArray(esb, nelems);

				scratch.d.minmax.nelems = nelems;

				scratch.d.minmax.op = minmaxexpr->op;
				scratch.d.minmax.fcinfo_data = fcinfo_rel;
				scratch.d.minmax.fn_addr = finfo->fn_addr;

				/* evaluate expressions into minmax->values/nulls */
				off = 0;
				foreach(lc, minmaxexpr->args)
				{
					Expr	   *e = (Expr *) lfirst(lc);

					ExecInitExprRec(esb, e, RelArrayIdx(scratch.d.minmax.arguments, off));
					off++;
				}

				/* and push the final comparison */
				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_SQLValueFunction:
			{
				SQLValueFunction *svf = (SQLValueFunction *) node;

				scratch.opcode = EEOP_SQLVALUEFUNCTION;
				scratch.d.sqlvaluefunction.svf = svf;

				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_XmlExpr:
			{
				XmlExpr    *xexpr = (XmlExpr *) node;
				int			nnamed = list_length(xexpr->named_args);
				int			nargs = list_length(xexpr->args);
				int			off;
				ListCell   *arg;

				scratch.opcode = EEOP_XMLEXPR;
				scratch.d.xmlexpr.xexpr = xexpr;

				/* allocate space for storing all the arguments */
				if (nnamed)
				{
					scratch.d.xmlexpr.named_args =
						ExprBuilderAllocNullableDatumArray(esb, nnamed);
				}
				else
				{
					scratch.d.xmlexpr.named_args.ptr.allocno = 0;
				}

				if (nargs)
				{
					scratch.d.xmlexpr.args =
						ExprBuilderAllocNullableDatumArray(esb, nargs);
				}
				else
				{
					scratch.d.xmlexpr.args.ptr.allocno = 0;
				}

				/* prepare argument execution */
				off = 0;
				foreach(arg, xexpr->named_args)
				{
					Expr	   *e = (Expr *) lfirst(arg);

					ExecInitExprRec(esb, e, RelArrayIdx(scratch.d.xmlexpr.named_args, off));
					off++;
				}

				off = 0;
				foreach(arg, xexpr->args)
				{
					Expr	   *e = (Expr *) lfirst(arg);

					ExecInitExprRec(esb, e, RelArrayIdx(scratch.d.xmlexpr.args, off));
					off++;
				}

				/* and evaluate the actual XML expression */
				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_NullTest:
			{
				NullTest   *ntest = (NullTest *) node;

				if (ntest->nulltesttype == IS_NULL)
				{
					if (ntest->argisrow)
						scratch.opcode = EEOP_NULLTEST_ROWISNULL;
					else
						scratch.opcode = EEOP_NULLTEST_ISNULL;
				}
				else if (ntest->nulltesttype == IS_NOT_NULL)
				{
					if (ntest->argisrow)
						scratch.opcode = EEOP_NULLTEST_ROWISNOTNULL;
					else
						scratch.opcode = EEOP_NULLTEST_ISNOTNULL;
				}
				else
				{
					elog(ERROR, "unrecognized nulltesttype: %d",
						 (int) ntest->nulltesttype);
				}
				/* initialize cache in case it's a row test */
				scratch.d.nulltest_row.argdesc = NULL;

				/* first evaluate argument into result variable */
				ExecInitExprRec(esb, ntest->arg, result);

				/* then push the test of that argument */
				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_BooleanTest:
			{
				BooleanTest *btest = (BooleanTest *) node;

				/*
				 * Evaluate argument, directly into result datum.  That's ok,
				 * because result is definitely not used anywhere else,
				 * and will get overwritten by the below EEOP_BOOLTEST_IS_*
				 * step.
				 */
				ExecInitExprRec(esb, btest->arg, result);

				switch (btest->booltesttype)
				{
					case IS_TRUE:
						scratch.opcode = EEOP_BOOLTEST_IS_TRUE;
						break;
					case IS_NOT_TRUE:
						scratch.opcode = EEOP_BOOLTEST_IS_NOT_TRUE;
						break;
					case IS_FALSE:
						scratch.opcode = EEOP_BOOLTEST_IS_FALSE;
						break;
					case IS_NOT_FALSE:
						scratch.opcode = EEOP_BOOLTEST_IS_NOT_FALSE;
						break;
					case IS_UNKNOWN:
						/* Same as scalar IS NULL test */
						scratch.opcode = EEOP_NULLTEST_ISNULL;
						break;
					case IS_NOT_UNKNOWN:
						/* Same as scalar IS NOT NULL test */
						scratch.opcode = EEOP_NULLTEST_ISNOTNULL;
						break;
					default:
						elog(ERROR, "unrecognized booltesttype: %d",
							 (int) btest->booltesttype);
				}

				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_CoerceToDomain:
			{
				CoerceToDomain *ctest = (CoerceToDomain *) node;

				ExecInitCoerceToDomain(&scratch, ctest, esb, result);
				break;
			}

		case T_CoerceToDomainValue:
			{
				/*
				 * Read from location identified by innermost_domainval.  Note
				 * that innermost_domainval could be NULL, if we're compiling
				 * a standalone domain check rather than one embedded in a
				 * larger expression.  In that case we must read from
				 * econtext->domainValue_datum.  We'll take care of that
				 * scenario at runtime.
				 */
				scratch.opcode = EEOP_DOMAIN_TESTVAL;
				/* we share instruction union variant with case testval */
				scratch.d.casetest.value = esb->innermost_domainval;

				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_CurrentOfExpr:
			{
				scratch.opcode = EEOP_CURRENTOFEXPR;
				ExprEvalPushStep(esb, &scratch);
				break;
			}

		case T_NextValueExpr:
			{
				NextValueExpr *nve = (NextValueExpr *) node;

				scratch.opcode = EEOP_NEXTVALUEEXPR;
				scratch.d.nextvalueexpr.seqid = nve->seqid;
				scratch.d.nextvalueexpr.seqtypid = nve->typeId;

				ExprEvalPushStep(esb, &scratch);
				break;
			}

		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(node));
			break;
	}
}

/*
 * Add another expression evaluation step to ExprStateBuilder->steps.
 *
 * Note that this potentially re-allocates es->steps, therefore no pointer
 * into that array may be used while the expression is still being built.
 */
static void
ExprEvalPushStep(ExprStateBuilder *esb, const ExprEvalStep *s)
{
	ExprEvalStep *as;

#if 0
	if (esb->steps_alloc == 0)
	{
		esb->steps_alloc = 16;
		esb->steps = palloc(sizeof(ExprEvalStep) * esb->steps_alloc);
	}
	else if (esb->steps_alloc == esb->steps_len)
	{
		esb->steps_alloc *= 2;
		esb->steps = repalloc(esb->steps,
							  sizeof(ExprEvalStep) * esb->steps_alloc);
	}

	memcpy(&esb->steps[esb->steps_len++], s, sizeof(ExprEvalStep));
#endif

	as = palloc(sizeof(ExprEvalStep));
	memcpy(as, s, sizeof(ExprEvalStep));

	esb->steps = lappend(esb->steps, as);
	esb->steps_len++;
}

/*
 * Perform setup necessary for the evaluation of a function-like expression,
 * appending argument evaluation steps to the steps list in *esb, and
 * setting up *scratch so it is ready to be pushed.
 *
 * *scratch is not pushed here, so that callers may override the opcode,
 * which is useful for function-like cases like DISTINCT.
 */
static void
ExecInitFunc(ExprEvalStep *scratch, Expr *node, List *args, Oid funcid,
			 Oid inputcollid, ExprStateBuilder *esb)
{
	int			nargs = list_length(args);
	AclResult	aclresult;
	FmgrInfo   *flinfo;
	RelFunctionCallInfo fcinfo_rel;
	FunctionCallInfo fcinfo;
	int			argno;
	ListCell   *lc;

	/* Check permission to call function */
	aclresult = pg_proc_aclcheck(funcid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(funcid));
	InvokeFunctionExecuteHook(funcid);

	/*
	 * Safety check on nargs.  Under normal circumstances this should never
	 * fail, as parser should check sooner.  But possibly it might fail if
	 * server has been compiled with FUNC_MAX_ARGS smaller than some functions
	 * declared in pg_proc?
	 */
	if (nargs > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg_plural("cannot pass more than %d argument to a function",
							   "cannot pass more than %d arguments to a function",
							   FUNC_MAX_ARGS,
							   FUNC_MAX_ARGS)));

	/* Allocate function lookup data and parameter workspace for this call */
	flinfo = palloc0(sizeof(FmgrInfo));
	fcinfo_rel = ExprBuilderAllocFunctionCallInfo(esb, nargs, &fcinfo);
	scratch->d.func.fcinfo_data = fcinfo_rel;

	/* Set up the primary fmgr lookup information */
	fmgr_info(funcid, flinfo);
	fmgr_info_set_expr((Node *) node, flinfo);

	/* Initialize function call parameter structure too */
	InitFunctionCallInfoData(*fcinfo, flinfo,
							 nargs, inputcollid, NULL, NULL);

	/* Keep extra copies of this info to save an indirection at runtime */
	scratch->d.func.fn_strict = flinfo->fn_strict;
	scratch->d.func.fn_addr = flinfo->fn_addr;
	scratch->d.func.nargs = nargs;

	/* We only support non-set functions here */
	if (flinfo->fn_retset)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set"),
				 esb->parent ?
				 executor_errposition(esb->parent->state,
									  exprLocation((Node *) node)) : 0));

	/* Build code to evaluate arguments directly into the fcinfo struct */
	argno = 0;
	foreach(lc, args)
	{
		Expr	   *arg = (Expr *) lfirst(lc);

		if (IsA(arg, Const))
		{
			/*
			 * Don't evaluate const arguments every round; especially
			 * interesting for constants in comparisons.
			 */
			Const	   *con = (Const *) arg;

			fcinfo->args[argno].value = con->constvalue;
			fcinfo->args[argno].isnull = con->constisnull;
		}
		else
		{
			ExecInitExprRec(esb, arg, RelFCIIdx(fcinfo_rel, argno));
		}
		argno++;
	}

	/* Insert appropriate opcode depending on strictness and stats level */
	if (pgstat_track_functions <= flinfo->fn_stats)
	{
		if (flinfo->fn_strict && nargs > 0)
		{
			/* Choose nargs optimized implementation if available. */
			if (nargs == 1)
				scratch->opcode = EEOP_FUNCEXPR_STRICT_1;
			else if (nargs == 2)
				scratch->opcode = EEOP_FUNCEXPR_STRICT_2;
			else
				scratch->opcode = EEOP_FUNCEXPR_STRICT;
		}
		else
			scratch->opcode = EEOP_FUNCEXPR;
	}
	else
	{
		if (flinfo->fn_strict && nargs > 0)
			scratch->opcode = EEOP_FUNCEXPR_STRICT_FUSAGE;
		else
			scratch->opcode = EEOP_FUNCEXPR_FUSAGE;
	}
}

/*
 * Add expression steps deforming the ExprState's inner/outer/scan slots
 * as much as required by the expression.
 */
static void
ExecInitExprSlots(ExprStateBuilder *esb, Node *node)
{
	LastAttnumInfo info = {0, 0, 0};

	/*
	 * Figure out which attributes we're going to need.
	 */
	get_last_attnums_walker(node, &info);

	ExecPushExprSlots(esb, &info);
}

/*
 * Add steps deforming the ExprState's inner/out/scan slots as much as
 * indicated by info. This is useful when building an ExprState covering more
 * than one expression.
 */
static void
ExecPushExprSlots(ExprStateBuilder *esb, LastAttnumInfo *info)
{
	ExprEvalStep scratch = {0};

	scratch.result.ptr.allocno = 0;

	/* Emit steps as needed */
	if (info->last_inner > 0)
	{
		scratch.opcode = EEOP_INNER_FETCHSOME;
		scratch.d.fetch.last_var = info->last_inner;
		scratch.d.fetch.fixed = false;
		scratch.d.fetch.kind = NULL;
		scratch.d.fetch.known_desc = NULL;
		if (ExecComputeSlotInfo(esb, &scratch))
			ExprEvalPushStep(esb, &scratch);
	}
	if (info->last_outer > 0)
	{
		scratch.opcode = EEOP_OUTER_FETCHSOME;
		scratch.d.fetch.last_var = info->last_outer;
		scratch.d.fetch.fixed = false;
		scratch.d.fetch.kind = NULL;
		scratch.d.fetch.known_desc = NULL;
		if (ExecComputeSlotInfo(esb, &scratch))
			ExprEvalPushStep(esb, &scratch);
	}
	if (info->last_scan > 0)
	{
		scratch.opcode = EEOP_SCAN_FETCHSOME;
		scratch.d.fetch.last_var = info->last_scan;
		scratch.d.fetch.fixed = false;
		scratch.d.fetch.kind = NULL;
		scratch.d.fetch.known_desc = NULL;
		if (ExecComputeSlotInfo(esb, &scratch))
			ExprEvalPushStep(esb, &scratch);
	}
}

/*
 * get_last_attnums_walker: expression walker for ExecInitExprSlots
 */
static bool
get_last_attnums_walker(Node *node, LastAttnumInfo *info)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *variable = (Var *) node;
		AttrNumber	attnum = variable->varattno;

		switch (variable->varno)
		{
			case INNER_VAR:
				info->last_inner = Max(info->last_inner, attnum);
				break;

			case OUTER_VAR:
				info->last_outer = Max(info->last_outer, attnum);
				break;

				/* INDEX_VAR is handled by default case */

			default:
				info->last_scan = Max(info->last_scan, attnum);
				break;
		}
		return false;
	}

	/*
	 * Don't examine the arguments or filters of Aggrefs or WindowFuncs,
	 * because those do not represent expressions to be evaluated within the
	 * calling expression's econtext.  GroupingFunc arguments are never
	 * evaluated at all.
	 */
	if (IsA(node, Aggref))
		return false;
	if (IsA(node, WindowFunc))
		return false;
	if (IsA(node, GroupingFunc))
		return false;
	return expression_tree_walker(node, get_last_attnums_walker,
								  (void *) info);
}

/*
 * Compute additional information for EEOP_*_FETCHSOME ops.
 *
 * The goal is to determine whether a slot is 'fixed', that is, every
 * evaluation of the expression will have the same type of slot, with an
 * equivalent descriptor.
 *
 * Returns true if the the deforming step is required, false otherwise.
 */
static bool
ExecComputeSlotInfo(ExprStateBuilder *esb, ExprEvalStep *op)
{
	PlanState  *parent = esb->parent;
	TupleDesc	desc = NULL;
	const TupleTableSlotOps *tts_ops = NULL;
	bool		isfixed = false;
	ExprEvalOp	opcode = op->opcode;

	Assert(opcode == EEOP_INNER_FETCHSOME ||
		   opcode == EEOP_OUTER_FETCHSOME ||
		   opcode == EEOP_SCAN_FETCHSOME);

	if (op->d.fetch.known_desc != NULL)
	{
		desc = op->d.fetch.known_desc;
		tts_ops = op->d.fetch.kind;
		isfixed = op->d.fetch.kind != NULL;
	}
	else if (!parent)
	{
		isfixed = false;
	}
	else if (opcode == EEOP_INNER_FETCHSOME)
	{
		PlanState  *is = innerPlanState(parent);

		if (parent->inneropsset && !parent->inneropsfixed)
		{
			isfixed = false;
		}
		else if (parent->inneropsset && parent->innerops)
		{
			isfixed = true;
			tts_ops = parent->innerops;
			desc = ExecGetResultType(is);
		}
		else if (is)
		{
			tts_ops = ExecGetResultSlotOps(is, &isfixed);
			desc = ExecGetResultType(is);
		}
	}
	else if (opcode == EEOP_OUTER_FETCHSOME)
	{
		PlanState  *os = outerPlanState(parent);

		if (parent->outeropsset && !parent->outeropsfixed)
		{
			isfixed = false;
		}
		else if (parent->outeropsset && parent->outerops)
		{
			isfixed = true;
			tts_ops = parent->outerops;
			desc = ExecGetResultType(os);
		}
		else if (os)
		{
			tts_ops = ExecGetResultSlotOps(os, &isfixed);
			desc = ExecGetResultType(os);
		}
	}
	else if (opcode == EEOP_SCAN_FETCHSOME)
	{
		desc = parent->scandesc;

		if (parent && parent->scanops)
			tts_ops = parent->scanops;

		if (parent->scanopsset)
			isfixed = parent->scanopsfixed;
	}

	if (isfixed && desc != NULL && tts_ops != NULL)
	{
		op->d.fetch.fixed = true;
		op->d.fetch.kind = tts_ops;
		op->d.fetch.known_desc = desc;
	}
	else
	{
		op->d.fetch.fixed = false;
		op->d.fetch.kind = NULL;
		op->d.fetch.known_desc = NULL;
	}

	/* if the slot is known to always virtual we never need to deform */
	if (op->d.fetch.fixed && op->d.fetch.kind == &TTSOpsVirtual)
		return false;

	return true;
}

/*
 * Prepare step for the evaluation of a whole-row variable.
 * The caller still has to push the step.
 */
static void
ExecInitWholeRowVar(ExprEvalStep *scratch, Var *variable, ExprStateBuilder *esb)
{
	PlanState  *parent = esb->parent;

	/* fill in all but the target */
	scratch->opcode = EEOP_WHOLEROW;
	scratch->d.wholerow.var = variable;
	scratch->d.wholerow.first = true;
	scratch->d.wholerow.slow = false;
	scratch->d.wholerow.tupdesc = NULL; /* filled at runtime */
	scratch->d.wholerow.junkFilter = NULL;

	/*
	 * If the input tuple came from a subquery, it might contain "resjunk"
	 * columns (such as GROUP BY or ORDER BY columns), which we don't want to
	 * keep in the whole-row result.  We can get rid of such columns by
	 * passing the tuple through a JunkFilter --- but to make one, we have to
	 * lay our hands on the subquery's targetlist.  Fortunately, there are not
	 * very many cases where this can happen, and we can identify all of them
	 * by examining our parent PlanState.  We assume this is not an issue in
	 * standalone expressions that don't have parent plans.  (Whole-row Vars
	 * can occur in such expressions, but they will always be referencing
	 * table rows.)
	 */
	if (parent)
	{
		PlanState  *subplan = NULL;

		switch (nodeTag(parent))
		{
			case T_SubqueryScanState:
				subplan = ((SubqueryScanState *) parent)->subplan;
				break;
			case T_CteScanState:
				subplan = ((CteScanState *) parent)->cteplanstate;
				break;
			default:
				break;
		}

		if (subplan)
		{
			bool		junk_filter_needed = false;
			ListCell   *tlist;

			/* Detect whether subplan tlist actually has any junk columns */
			foreach(tlist, subplan->plan->targetlist)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(tlist);

				if (tle->resjunk)
				{
					junk_filter_needed = true;
					break;
				}
			}

			/* If so, build the junkfilter now */
			if (junk_filter_needed)
			{
				scratch->d.wholerow.junkFilter =
					ExecInitJunkFilter(subplan->plan->targetlist,
									   ExecInitExtraTupleSlot(parent->state, NULL,
															  &TTSOpsVirtual));
			}
		}
	}
}

/*
 * Prepare evaluation of a SubscriptingRef expression.
 */
static void
ExecInitSubscriptingRef(ExprEvalStep *scratch, SubscriptingRef *sbsref,
						ExprStateBuilder *esb, RelNullableDatum result)
{
	bool		isAssignment = (sbsref->refassgnexpr != NULL);
	SubscriptingRefState *sbsrefstate = palloc0(sizeof(SubscriptingRefState));
	RelNullableDatum subscript;
	RelNullableDatum replace;
	RelNullableDatum prev;
	List	   *adjust_jumps = NIL;
	ListCell   *lc;
	int			i;

	/* Fill constant fields of SubscriptingRefState */
	sbsrefstate->isassignment = isAssignment;
	sbsrefstate->refelemtype = sbsref->refelemtype;
	sbsrefstate->refattrlength = get_typlen(sbsref->refcontainertype);
	get_typlenbyvalalign(sbsref->refelemtype,
						 &sbsrefstate->refelemlength,
						 &sbsrefstate->refelembyval,
						 &sbsrefstate->refelemalign);

	subscript = ExprBuilderAllocNullableDatum(esb);
	replace = ExprBuilderAllocNullableDatum(esb);
	prev = ExprBuilderAllocNullableDatum(esb);

	/*
	 * Evaluate array input.  It's safe to do so into result, because we
	 * won't use that as target for any of the other subexpressions, and it'll
	 * be overwritten by the final EEOP_SBSREF_FETCH/ASSIGN step, which is
	 * pushed last.
	 */
	ExecInitExprRec(esb, sbsref->refexpr, result);

	/*
	 * If refexpr yields NULL, and it's a fetch, then result is NULL.  We can
	 * implement this with just JUMP_IF_NULL, since we evaluated the array
	 * into the desired target location.
	 */
	if (!isAssignment)
	{
		scratch->opcode = EEOP_JUMP_IF_NULL;
		scratch->d.jump.jumpdone = -1;	/* adjust later */
		ExprEvalPushStep(esb, scratch);
		adjust_jumps = lappend_int(adjust_jumps,
								   esb->steps_len - 1);
	}

	/* Verify subscript list lengths are within limit */
	if (list_length(sbsref->refupperindexpr) > MAXDIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
						list_length(sbsref->refupperindexpr), MAXDIM)));

	if (list_length(sbsref->reflowerindexpr) > MAXDIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
						list_length(sbsref->reflowerindexpr), MAXDIM)));

	/* Evaluate upper subscripts */
	i = 0;
	foreach(lc, sbsref->refupperindexpr)
	{
		Expr	   *e = (Expr *) lfirst(lc);

		/* When slicing, individual subscript bounds can be omitted */
		if (!e)
		{
			sbsrefstate->upperprovided[i] = false;
			i++;
			continue;
		}

		sbsrefstate->upperprovided[i] = true;

		/* Each subscript is evaluated into subscriptvalue/subscriptnull */
		ExecInitExprRec(esb, e, subscript);

		/* ... and then SBSREF_SUBSCRIPT saves it into step's workspace */
		scratch->opcode = EEOP_SBSREF_SUBSCRIPT;
		scratch->d.sbsref_subscript.state = sbsrefstate;
		scratch->d.sbsref_subscript.subscript = subscript;
		scratch->d.sbsref_subscript.off = i;
		scratch->d.sbsref_subscript.isupper = true;
		scratch->d.sbsref_subscript.jumpdone = -1;	/* adjust later */
		ExprEvalPushStep(esb, scratch);
		adjust_jumps = lappend_int(adjust_jumps,
								   esb->steps_len - 1);
		i++;
	}
	sbsrefstate->numupper = i;

	/* Evaluate lower subscripts similarly */
	i = 0;
	foreach(lc, sbsref->reflowerindexpr)
	{
		Expr	   *e = (Expr *) lfirst(lc);

		/* When slicing, individual subscript bounds can be omitted */
		if (!e)
		{
			sbsrefstate->lowerprovided[i] = false;
			i++;
			continue;
		}

		sbsrefstate->lowerprovided[i] = true;

		/* Each subscript is evaluated into subscriptvalue/subscriptnull */
		ExecInitExprRec(esb, e, subscript);

		/* ... and then SBSREF_SUBSCRIPT saves it into step's workspace */
		scratch->opcode = EEOP_SBSREF_SUBSCRIPT;
		scratch->d.sbsref_subscript.state = sbsrefstate;
		scratch->d.sbsref_subscript.subscript = subscript;
		scratch->d.sbsref_subscript.off = i;
		scratch->d.sbsref_subscript.isupper = false;
		scratch->d.sbsref_subscript.jumpdone = -1;	/* adjust later */
		ExprEvalPushStep(esb, scratch);
		adjust_jumps = lappend_int(adjust_jumps,
								   esb->steps_len - 1);
		i++;
	}
	sbsrefstate->numlower = i;

	/* Should be impossible if parser is sane, but check anyway: */
	if (sbsrefstate->numlower != 0 &&
		sbsrefstate->numupper != sbsrefstate->numlower)
		elog(ERROR, "upper and lower index lists are not same length");

	if (isAssignment)
	{
		RelNullableDatum save_innermost_caseval;

		/*
		 * We might have a nested-assignment situation, in which the
		 * refassgnexpr is itself a FieldStore or SubscriptingRef that needs
		 * to obtain and modify the previous value of the array element or
		 * slice being replaced.  If so, we have to extract that value from
		 * the array and pass it down via the CaseTestExpr mechanism.  It's
		 * safe to reuse the CASE mechanism because there cannot be a CASE
		 * between here and where the value would be needed, and an array
		 * assignment can't be within a CASE either.  (So saving and restoring
		 * innermost_caseval is just paranoia, but let's do it anyway.)
		 *
		 * Since fetching the old element might be a nontrivial expense, do it
		 * only if the argument actually needs it.
		 */
		if (isAssignmentIndirectionExpr(sbsref->refassgnexpr))
		{
			scratch->opcode = EEOP_SBSREF_OLD;
			scratch->d.sbsref.state = sbsrefstate;
			scratch->d.sbsref.replace = replace;
			scratch->d.sbsref.prev = prev;
			ExprEvalPushStep(esb, scratch);
		}

		/* SBSREF_OLD puts extracted value into prevvalue/prevnull */
		save_innermost_caseval = esb->innermost_caseval;
		esb->innermost_caseval = prev;

		/* evaluate replacement value into replacevalue/replacenull */
		ExecInitExprRec(esb, sbsref->refassgnexpr, replace);

		esb->innermost_caseval = save_innermost_caseval;

		/* and perform the assignment */
		scratch->opcode = EEOP_SBSREF_ASSIGN;
		scratch->d.sbsref.replace = replace;
		scratch->d.sbsref.prev = prev;
		scratch->d.sbsref.state = sbsrefstate;
		ExprEvalPushStep(esb, scratch);

	}
	else
	{
		/* array fetch is much simpler */
		scratch->opcode = EEOP_SBSREF_FETCH;
		scratch->d.sbsref.replace = replace;
		scratch->d.sbsref.prev = prev;
		scratch->d.sbsref.state = sbsrefstate;
		ExprEvalPushStep(esb, scratch);

	}

	/* adjust jump targets */
	foreach(lc, adjust_jumps)
	{
		ExprEvalStep *as = nth_step(esb, lfirst_int(lc));

		if (as->opcode == EEOP_SBSREF_SUBSCRIPT)
		{
			Assert(as->d.sbsref_subscript.jumpdone == -1);
			as->d.sbsref_subscript.jumpdone = esb->steps_len;
		}
		else
		{
			Assert(as->opcode == EEOP_JUMP_IF_NULL);
			Assert(as->d.jump.jumpdone == -1);
			as->d.jump.jumpdone = esb->steps_len;
		}
	}
}

/*
 * Helper for preparing SubscriptingRef expressions for evaluation: is expr
 * a nested FieldStore or SubscriptingRef that needs the old element value
 * passed down?
 *
 * (We could use this in FieldStore too, but in that case passing the old
 * value is so cheap there's no need.)
 *
 * Note: it might seem that this needs to recurse, but it does not; the
 * CaseTestExpr, if any, will be directly the arg or refexpr of the top-level
 * node.  Nested-assignment situations give rise to expression trees in which
 * each level of assignment has its own CaseTestExpr, and the recursive
 * structure appears within the newvals or refassgnexpr field.
 */
static bool
isAssignmentIndirectionExpr(Expr *expr)
{
	if (expr == NULL)
		return false;			/* just paranoia */
	if (IsA(expr, FieldStore))
	{
		FieldStore *fstore = (FieldStore *) expr;

		if (fstore->arg && IsA(fstore->arg, CaseTestExpr))
			return true;
	}
	else if (IsA(expr, SubscriptingRef))
	{
		SubscriptingRef *sbsRef = (SubscriptingRef *) expr;

		if (sbsRef->refexpr && IsA(sbsRef->refexpr, CaseTestExpr))
			return true;
	}
	return false;
}

/*
 * Prepare evaluation of a CoerceToDomain expression.
 */
static void
ExecInitCoerceToDomain(ExprEvalStep *scratch, CoerceToDomain *ctest,
					   ExprStateBuilder *esb, RelNullableDatum result)
{
	DomainConstraintRef *constraint_ref;
	ListCell   *l;

	scratch->d.domaincheck.resulttype = ctest->resulttype;
	/* we'll allocate workspace only if needed */
	scratch->d.domaincheck.check.ptr.allocno = 0;

	/*
	 * Evaluate argument - it's fine to directly store it into result, if
	 * there's constraint failures there'll be errors, otherwise it's what
	 * needs to be returned.
	 */
	ExecInitExprRec(esb, ctest->arg, result);

	/*
	 * Note: if the argument is of varlena type, it could be a R/W expanded
	 * object.  We want to return the R/W pointer as the final result, but we
	 * have to pass a R/O pointer as the value to be tested by any functions
	 * in check expressions.  We don't bother to emit a MAKE_READONLY step
	 * unless there's actually at least one check expression, though.  Until
	 * we've tested that, domainval/domainnull are NULL.
	 */

	/*
	 * Collect the constraints associated with the domain.
	 *
	 * Note: before PG v10 we'd recheck the set of constraints during each
	 * evaluation of the expression.  Now we bake them into the ExprState
	 * during executor initialization.  That means we don't need typcache.c to
	 * provide compiled exprs.
	 */
	constraint_ref = (DomainConstraintRef *)
		palloc(sizeof(DomainConstraintRef));
	InitDomainConstraintRef(ctest->resulttype,
							constraint_ref,
							CurrentMemoryContext,
							false);

	/*
	 * Compile code to check each domain constraint.  NOTNULL constraints can
	 * just be applied on the result value, but for CHECK constraints we
	 * need more pushups.
	 */
	foreach(l, constraint_ref->constraints)
	{
		DomainConstraintState *con = (DomainConstraintState *) lfirst(l);
		RelNullableDatum domainval = {0};
		RelNullableDatum save_innermost_domainval;

		scratch->d.domaincheck.constraintname = con->name;

		switch (con->constrainttype)
		{
			case DOM_CONSTRAINT_NOTNULL:
				scratch->opcode = EEOP_DOMAIN_NOTNULL;
				ExprEvalPushStep(esb, scratch);
				break;
			case DOM_CONSTRAINT_CHECK:
				/* Allocate workspace for CHECK output if we didn't yet */
				if (scratch->d.domaincheck.check.ptr.allocno == 0)
					scratch->d.domaincheck.check =
						ExprBuilderAllocNullableDatum(esb);

				/*
				 * If first time through, determine where CoerceToDomainValue
				 * nodes should read from.
				 */
				if (domainval.ptr.allocno == 0)
				{
					/*
					 * Since value might be read multiple times, force to R/O
					 * - but only if it could be an expanded datum.
					 */
					if (get_typlen(ctest->resulttype) == -1)
					{
						ExprEvalStep scratch2 = {0};

						/* Yes, so make output workspace for MAKE_READONLY */
						domainval =
							ExprBuilderAllocNullableDatum(esb);

						/* Emit MAKE_READONLY */
						scratch2.opcode = EEOP_MAKE_READONLY;
						scratch2.result = domainval;
						scratch2.d.make_readonly.value = result;
						ExprEvalPushStep(esb, &scratch2);
					}
					else
					{
						/* No, so it's fine to read from result */
						domainval = result;
					}
				}

				/*
				 * Set up value to be returned by CoerceToDomainValue nodes.
				 * We must save and restore innermost_domainval/null fields,
				 * in case this node is itself within a check expression for
				 * another domain.
				 */
				save_innermost_domainval = esb->innermost_domainval;
				esb->innermost_domainval = domainval;

				/* evaluate check expression value */
				ExecInitExprRec(esb, con->check_expr,
								scratch->d.domaincheck.check);

				esb->innermost_domainval = save_innermost_domainval;

				/* now test result */
				scratch->opcode = EEOP_DOMAIN_CHECK;
				ExprEvalPushStep(esb, scratch);

				break;
			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 (int) con->constrainttype);
				break;
		}
	}
}

/*
 * Build transition/combine function invocations for all aggregate transition
 * / combination function invocations in a grouping sets phase. This has to
 * invoke all sort based transitions in a phase (if doSort is true), all hash
 * based transitions (if doHash is true), or both (both true).
 *
 * The resulting expression will, for each set of transition values, first
 * check for filters, evaluate aggregate input, check that that input is not
 * NULL for a strict transition function, and then finally invoke the
 * transition for each of the concurrently computed grouping sets.
 */
ExprState *
ExecBuildAggTrans(AggState *aggstate, AggStatePerPhase phase,
				  bool doSort, bool doHash)
{
	ExprStateBuilder esb = {0};
	PlanState  *parent = &aggstate->ss.ps;
	ExprEvalStep scratch = {0};
	bool		isCombine = DO_AGGSPLIT_COMBINE(aggstate->aggsplit);
	LastAttnumInfo deform = {0, 0, 0};

	esb.result = ExprBuilderAllocNullableDatum(&esb);
	esb.expr = (Expr *) aggstate;
	esb.parent = parent;

	// FIXME: should be able to get rid of this?
	scratch.result = esb.result;

	/*
	 * First figure out which slots, and how many columns from each, we're
	 * going to need.
	 */
	for (int transno = 0; transno < aggstate->numtrans; transno++)
	{
		AggStatePerTrans pertrans = &aggstate->pertrans[transno];

		get_last_attnums_walker((Node *) pertrans->aggref->aggdirectargs,
								&deform);
		get_last_attnums_walker((Node *) pertrans->aggref->args,
								&deform);
		get_last_attnums_walker((Node *) pertrans->aggref->aggorder,
								&deform);
		get_last_attnums_walker((Node *) pertrans->aggref->aggdistinct,
								&deform);
		get_last_attnums_walker((Node *) pertrans->aggref->aggfilter,
								&deform);
	}
	ExecPushExprSlots(&esb, &deform);

	/*
	 * Emit instructions for each transition value / grouping set combination.
	 */
	for (int transno = 0; transno < aggstate->numtrans; transno++)
	{
		AggStatePerTrans pertrans = &aggstate->pertrans[transno];
		RelFunctionCallInfo trans_fcinfo_rel;
		FunctionCallInfo trans_fcinfo;
		List	   *adjust_bailout = NIL;
		RelNullableDatumArray columns = {0};
		RelNullableDatumArray strictargs = {0};
		int			argno;
		ListCell   *bail;
		size_t		numTransArgs;

		/* account for the current transition state */
		numTransArgs = pertrans->numTransInputs + 1;
		trans_fcinfo_rel =
			ExprBuilderAllocFunctionCallInfo(&esb, numTransArgs,
											 &trans_fcinfo);
		InitFunctionCallInfoData(*trans_fcinfo,
								 &pertrans->transfn,
								 numTransArgs,
								 pertrans->aggCollation,
								 NULL, /* will get set when calling */
								 NULL);

		/*
		 * If filter present, emit. Do so before evaluating the input, to
		 * avoid potentially unneeded computations, or even worse, unintended
		 * side-effects.  When combining, all the necessary filtering has
		 * already been done.
		 */
		if (pertrans->aggref->aggfilter && !isCombine)
		{
			/* evaluate filter expression */
			ExecInitExprRec(&esb, pertrans->aggref->aggfilter, esb.result);
			/* and jump out if false */
			scratch.opcode = EEOP_JUMP_IF_NOT_TRUE;
			scratch.d.jump.jumpdone = -1;	/* adjust later */
			ExprEvalPushStep(&esb, &scratch);
			adjust_bailout = lappend_int(adjust_bailout,
										 esb.steps_len - 1);
		}

		/*
		 * Evaluate arguments to aggregate/combine function.
		 */
		argno = 0;
		if (isCombine)
		{
			/*
			 * Combining two aggregate transition values. Instead of directly
			 * coming from a tuple the input is a, potentially deserialized,
			 * transition value.
			 */
			TargetEntry *source_tle;

			Assert(pertrans->numSortCols == 0);
			Assert(list_length(pertrans->aggref->args) == 1);

			strictargs = RelFCIOff(trans_fcinfo_rel, 1);
			source_tle = (TargetEntry *) linitial(pertrans->aggref->args);

			/*
			 * deserialfn_oid will be set if we must deserialize the input
			 * state before calling the combine function.
			 */
			if (!OidIsValid(pertrans->deserialfn_oid))
			{
				/*
				 * Start from 1, since the 0th arg will be the transition
				 * value
				 */
				ExecInitExprRec(&esb, source_tle->expr,
								RelFCIIdx(trans_fcinfo_rel, argno + 1));
			}
			else
			{
				FunctionCallInfo ds_fcinfo;
				RelFunctionCallInfo ds_fcinfo_rel;
				AggStatePerCallContext *percall;

				percall = makeNode(AggStatePerCallContext);
				percall->aggstate = aggstate;
				percall->pertrans = pertrans;

				ds_fcinfo_rel =
					ExprBuilderAllocFunctionCallInfo(&esb, numTransArgs,
													 &ds_fcinfo);

				InitFunctionCallInfoData(*ds_fcinfo,
										 &pertrans->deserialfn,
										 2,
										 InvalidOid,
										 (void *) percall, NULL);

				/* evaluate argument */
				ExecInitExprRec(&esb, source_tle->expr,
								RelFCIIdx(ds_fcinfo_rel, 0));

				/* Dummy second argument for type-safety reasons */
				ds_fcinfo->args[1].value = PointerGetDatum(NULL);
				ds_fcinfo->args[1].isnull = false;

				/*
				 * Don't call a strict deserialization function with NULL
				 * input
				 */
				if (pertrans->deserialfn.fn_strict)
					scratch.opcode = EEOP_AGG_STRICT_DESERIALIZE;
				else
					scratch.opcode = EEOP_AGG_DESERIALIZE;

				scratch.d.agg_deserialize.fn_addr =
					pertrans->deserialfn.fn_addr;
				scratch.d.agg_deserialize.fcinfo_data = ds_fcinfo_rel;
				scratch.d.agg_deserialize.jumpnull = -1;	/* adjust later */
				scratch.result = RelFCIIdx(trans_fcinfo_rel, argno + 1);

				ExprEvalPushStep(&esb, &scratch);
				adjust_bailout = lappend_int(adjust_bailout,
											 esb.steps_len - 1);

				/* restore normal settings of scratch fields */
				scratch.result = esb.result;
			}
			argno++;
		}
		else if (pertrans->numSortCols == 0)
		{
			ListCell   *arg;

			/*
			 * Normal transition function without ORDER BY / DISTINCT.
			 */
			strictargs = RelFCIOff(trans_fcinfo_rel, 1);

			foreach(arg, pertrans->aggref->args)
			{
				TargetEntry *source_tle = (TargetEntry *) lfirst(arg);

				/*
				 * Start from 1, since the 0th arg will be the transition
				 * value
				 */
				ExecInitExprRec(&esb, source_tle->expr,
								RelFCIIdx(trans_fcinfo_rel, argno + 1));
				argno++;
			}
		}
		else if (pertrans->numInputs == 1)
		{
			/*
			 * DISTINCT and/or ORDER BY case, with a single column sorted on.
			 */
			TargetEntry *source_tle =
			(TargetEntry *) linitial(pertrans->aggref->args);

			Assert(list_length(pertrans->aggref->args) == 1);
			Assert(pertrans->numTransInputs <= 1);

			columns = ExprBuilderAllocNullableDatumArray(&esb, 1);
			strictargs = columns;

			ExecInitExprRec(&esb, source_tle->expr,
							RelArrayIdx(columns, 0));

			argno++;
		}
		else
		{
			/*
			 * DISTINCT and/or ORDER BY case, with multiple columns sorted on.
			 */
			ListCell   *arg;

			columns = ExprBuilderAllocNullableDatumArray(&esb, pertrans->numInputs);
			strictargs = columns;

			foreach(arg, pertrans->aggref->args)
			{
				TargetEntry *source_tle = (TargetEntry *) lfirst(arg);

				ExecInitExprRec(&esb, source_tle->expr,
								RelArrayIdx(columns, argno));
				argno++;
			}
		}
		Assert(pertrans->numInputs == argno);

		/*
		 * For a strict transfn, nothing happens when there's a NULL input; we
		 * just keep the prior transValue. This is true for both plain and
		 * sorted/distinct aggregates.
		 */
		if (trans_fcinfo->flinfo->fn_strict && pertrans->numTransInputs > 0)
		{
			if (pertrans->numTransInputs == 1)
				scratch.opcode = EEOP_AGG_STRICT_INPUT_CHECK_ARGS_1;
			else
				scratch.opcode = EEOP_AGG_STRICT_INPUT_CHECK_ARGS;
			scratch.d.agg_strict_input_check.args = strictargs;
			scratch.d.agg_strict_input_check.jumpnull = -1; /* adjust later */
			scratch.d.agg_strict_input_check.nargs = pertrans->numTransInputs;
			ExprEvalPushStep(&esb, &scratch);
			adjust_bailout = lappend_int(adjust_bailout,
										 esb.steps_len - 1);
		}

		/*
		 * Call transition function (once for each concurrently evaluated
		 * grouping set). Do so for both sort and hash based computations, as
		 * applicable.
		 */
		if (doSort)
		{
			int			processGroupingSets = Max(phase->numsets, 1);
			int			setoff = 0;

			for (int setno = 0; setno < processGroupingSets; setno++)
			{
				ExecBuildAggTransCall(&esb, aggstate, &scratch,
									  trans_fcinfo, trans_fcinfo_rel,
									  columns,
									  pertrans, transno, setno, setoff, false);
				setoff++;
			}
		}

		if (doHash)
		{
			int			numHashes = aggstate->num_hashes;
			int			setoff;

			/* in MIXED mode, there'll be preceding transition values */
			if (aggstate->aggstrategy != AGG_HASHED)
				setoff = aggstate->maxsets;
			else
				setoff = 0;

			for (int setno = 0; setno < numHashes; setno++)
			{
				ExecBuildAggTransCall(&esb, aggstate, &scratch,
									  trans_fcinfo, trans_fcinfo_rel,
									  columns,
									  pertrans, transno, setno, setoff, true);
				setoff++;
			}
		}

		/* adjust early bail out jump target(s) */
		foreach(bail, adjust_bailout)
		{
			ExprEvalStep *as = nth_step(&esb, lfirst_int(bail));

			if (as->opcode == EEOP_JUMP_IF_NOT_TRUE)
			{
				Assert(as->d.jump.jumpdone == -1);
				as->d.jump.jumpdone = esb.steps_len;
			}
			else if (as->opcode == EEOP_AGG_STRICT_INPUT_CHECK_ARGS ||
					 as->opcode == EEOP_AGG_STRICT_INPUT_CHECK_ARGS_1)
			{
				Assert(as->d.agg_strict_input_check.jumpnull == -1);
				as->d.agg_strict_input_check.jumpnull = esb.steps_len;
			}
			else if (as->opcode == EEOP_AGG_STRICT_DESERIALIZE)
			{
				Assert(as->d.agg_deserialize.jumpnull == -1);
				as->d.agg_deserialize.jumpnull = esb.steps_len;
			}
			else
				Assert(false);
		}
	}

	scratch.opcode = EEOP_DONE_NO_RETURN;
	scratch.result.ptr.allocno = 0;
	ExprEvalPushStep(&esb, &scratch);

	return ExecReadyExpr(&esb);
}

/*
 * Build transition/combine function invocation for a single transition
 * value. This is separated from ExecBuildAggTrans() because there are
 * multiple callsites (hash and sort in some grouping set cases).
 */
static void
ExecBuildAggTransCall(ExprStateBuilder *esb, AggState *aggstate,
					  ExprEvalStep *scratch,
					  FunctionCallInfo fcinfo, RelFunctionCallInfo fcinfo_rel,
					  RelNullableDatumArray columns,
					  AggStatePerTrans pertrans,
					  int transno, int setno, int setoff, bool ishash)
{
	AggStatePerCallContext *percall;

	percall = makeNode(AggStatePerCallContext);
	percall->aggstate = aggstate;
	percall->pertrans = pertrans;
	if (ishash)
		percall->aggcontext = aggstate->hashcontext;
	else
		percall->aggcontext = aggstate->aggcontexts[setno];
	percall->setno = setno;

	/*
	 * Determine appropriate transition implementation.
	 *
	 * For non-ordered aggregates:
	 *
	 * If the initial value for the transition state doesn't exist in the
	 * pg_aggregate table then we will let the first non-NULL value returned
	 * from the outer procNode become the initial value. (This is useful for
	 * aggregates like max() and min().) The noTransValue flag signals that we
	 * still need to do this. To do so we generate the
	 * EEOP_AGG_INIT_STRICT_PLAIN_TRANS{,_BYVAL} step. This step also needs to
	 * do the work described next:
	 *
	 * If the function is strict, but does have an initial value, choose
	 * EEOP_AGG_STRICT_PLAIN_TRANS{,_BYVAL}, which skips the transition
	 * function if the transition value has become NULL (because a previous
	 * transition function returned NULL). This step also needs to do the work
	 * described next:
	 *
	 * Otherwise we call EEOP_AGG_PLAIN_TRANS{,_BYVAL}, which does not have to
	 * perform either of the above checks.
	 *
	 * Having steps with overlapping responsibilities is not nice, but
	 * aggregations are very performance sensitive, making this worthwhile.
	 *
	 * For ordered aggregates:
	 *
	 * Only need to choose between the faster path for a single orderred
	 * column, and the one between multiple columns. Checking strictness etc
	 * is done when finalizing the aggregate. See
	 * process_ordered_aggregate_{single, multi} and
	 * advance_transition_function.
	 */
	if (pertrans->numSortCols == 0)
	{
		if (pertrans->transtypeByVal)
		{
			if (fcinfo->flinfo->fn_strict &&
				pertrans->initValue.isnull)
				scratch->opcode = EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL;
			else if (fcinfo->flinfo->fn_strict)
				scratch->opcode = EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL;
			else
				scratch->opcode = EEOP_AGG_PLAIN_TRANS_BYVAL;
		}
		else
		{
			if (fcinfo->flinfo->fn_strict &&
				pertrans->initValue.isnull)
				scratch->opcode = EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF;
			else if (fcinfo->flinfo->fn_strict)
				scratch->opcode = EEOP_AGG_PLAIN_TRANS_STRICT_BYREF;
			else
				scratch->opcode = EEOP_AGG_PLAIN_TRANS_BYREF;
		}

		scratch->d.agg_trans.percall = percall;
		scratch->d.agg_trans.trans_fcinfo = fcinfo_rel;
		scratch->d.agg_trans.fn_addr = fcinfo->flinfo->fn_addr;
		scratch->d.agg_trans.setno = setno;
		scratch->d.agg_trans.setoff = setoff;
		scratch->d.agg_trans.transno = transno;
	}
	else
	{
		if (pertrans->numInputs == 1)
			scratch->opcode = EEOP_AGG_ORDERED_TRANS_DATUM;
		else
			scratch->opcode = EEOP_AGG_ORDERED_TRANS_TUPLE;

		scratch->d.agg_trans_ordered.pertrans = pertrans;
		scratch->d.agg_trans_ordered.setno = setno;
		scratch->d.agg_trans_ordered.columns = columns;
	}

	ExprEvalPushStep(esb, scratch);
}

/*
 * Build equality expression that can be evaluated using ExecQual(), returning
 * true if the expression context's inner/outer tuple are NOT DISTINCT. I.e
 * two nulls match, a null and a not-null don't match.
 *
 * desc: tuple descriptor of the to-be-compared tuples
 * numCols: the number of attributes to be examined
 * keyColIdx: array of attribute column numbers
 * eqFunctions: array of function oids of the equality functions to use
 * parent: parent executor node
 */
ExprState *
ExecBuildGroupingEqual(TupleDesc ldesc, TupleDesc rdesc,
					   const TupleTableSlotOps *lops, const TupleTableSlotOps *rops,
					   int numCols,
					   const AttrNumber *keyColIdx,
					   const Oid *eqfunctions,
					   const Oid *collations,
					   PlanState *parent)
{
	ExprStateBuilder esb = {0};
	ExprEvalStep scratch = {0};
	int			maxatt = -1;
	List	   *adjust_jumps = NIL;
	ListCell   *lc;

	/*
	 * When no columns are actually compared, the result's always true. See
	 * special case in ExecQual().
	 */
	if (numCols == 0)
		return NULL;

	esb.expr = NULL;
	esb.flags = EEO_FLAG_IS_QUAL;
	esb.parent = parent;

	esb.result = ExprBuilderAllocNullableDatum(&esb);
	scratch.result = esb.result;

	/* compute max needed attribute */
	for (int natt = 0; natt < numCols; natt++)
	{
		int			attno = keyColIdx[natt];

		if (attno > maxatt)
			maxatt = attno;
	}
	Assert(maxatt >= 0);

	/* push deform steps */
	scratch.opcode = EEOP_INNER_FETCHSOME;
	scratch.d.fetch.last_var = maxatt;
	scratch.d.fetch.fixed = false;
	scratch.d.fetch.known_desc = ldesc;
	scratch.d.fetch.kind = lops;
	if (ExecComputeSlotInfo(&esb, &scratch))
		ExprEvalPushStep(&esb, &scratch);

	scratch.opcode = EEOP_OUTER_FETCHSOME;
	scratch.d.fetch.last_var = maxatt;
	scratch.d.fetch.fixed = false;
	scratch.d.fetch.known_desc = rdesc;
	scratch.d.fetch.kind = rops;
	if (ExecComputeSlotInfo(&esb, &scratch))
		ExprEvalPushStep(&esb, &scratch);

	/*
	 * Start comparing at the last field (least significant sort key). That's
	 * the most likely to be different if we are dealing with sorted input.
	 */
	for (int natt = numCols; --natt >= 0;)
	{
		int			attno = keyColIdx[natt];
		Form_pg_attribute latt = TupleDescAttr(ldesc, attno - 1);
		Form_pg_attribute ratt = TupleDescAttr(rdesc, attno - 1);
		Oid			foid = eqfunctions[natt];
		Oid			collid = collations[natt];
		FmgrInfo   *finfo;
		RelFunctionCallInfo fcinfo_rel;
		FunctionCallInfo fcinfo;
		AclResult	aclresult;

		/* Check permission to call function */
		aclresult = pg_proc_aclcheck(foid, GetUserId(), ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(foid));

		InvokeFunctionExecuteHook(foid);

		/* Set up the primary fmgr lookup information */
		finfo = palloc0(sizeof(FmgrInfo));
		fcinfo_rel = ExprBuilderAllocFunctionCallInfo(&esb, 2, &fcinfo);
		fmgr_info(foid, finfo);
		fmgr_info_set_expr(NULL, finfo);
		InitFunctionCallInfoData(*fcinfo, finfo, 2,
								 collid, NULL, NULL);

		/* left arg */
		scratch.opcode = EEOP_INNER_VAR;
		scratch.d.var.attnum = attno - 1;
		scratch.d.var.vartype = latt->atttypid;
		scratch.result = RelFCIIdx(fcinfo_rel, 0);
		ExprEvalPushStep(&esb, &scratch);

		/* right arg */
		scratch.opcode = EEOP_OUTER_VAR;
		scratch.d.var.attnum = attno - 1;
		scratch.d.var.vartype = ratt->atttypid;
		scratch.result = RelFCIIdx(fcinfo_rel, 1);
		ExprEvalPushStep(&esb, &scratch);

		/* evaluate distinctness */
		scratch.opcode = EEOP_NOT_DISTINCT;
		scratch.d.func.fn_strict = finfo->fn_strict;
		scratch.d.func.fcinfo_data = fcinfo_rel;
		scratch.d.func.fn_addr = finfo->fn_addr;
		scratch.d.func.nargs = 2;
		scratch.result = esb.result;
		ExprEvalPushStep(&esb, &scratch);

		/* then emit EEOP_QUAL to detect if result is false (or null) */
		scratch.opcode = EEOP_QUAL;
		scratch.d.qualexpr.jumpdone = -1;
		scratch.result = esb.result;
		ExprEvalPushStep(&esb, &scratch);
		adjust_jumps = lappend_int(adjust_jumps,
								   esb.steps_len - 1);
	}

	/* adjust jump targets */
	foreach(lc, adjust_jumps)
	{
		ExprEvalStep *as = nth_step(&esb, lfirst_int(lc));

		Assert(as->opcode == EEOP_QUAL);
		Assert(as->d.qualexpr.jumpdone == -1);
		as->d.qualexpr.jumpdone = esb.steps_len;
	}

	scratch.result = esb.result;
	scratch.opcode = EEOP_DONE_RETURN;
	ExprEvalPushStep(&esb, &scratch);

	return ExecReadyExpr(&esb);
}

/* return string representation of op */
const char *
ExprOpToString(ExprEvalOp op)
{
	switch (op)
	{
		case EEOP_DONE_RETURN: return "EEOP_DONE_RETURN";
		case EEOP_DONE_NO_RETURN: return "EEOP_DONE_NO_RETURN";
		case EEOP_INNER_FETCHSOME: return "EEOP_INNER_FETCHSOME";
		case EEOP_OUTER_FETCHSOME: return "EEOP_OUTER_FETCHSOME";
		case EEOP_SCAN_FETCHSOME: return "EEOP_SCAN_FETCHSOME";
		case EEOP_INNER_VAR: return "EEOP_INNER_VAR";
		case EEOP_OUTER_VAR: return "EEOP_OUTER_VAR";
		case EEOP_SCAN_VAR: return "EEOP_SCAN_VAR";
		case EEOP_INNER_SYSVAR: return "EEOP_INNER_SYSVAR";
		case EEOP_OUTER_SYSVAR: return "EEOP_OUTER_SYSVAR";
		case EEOP_SCAN_SYSVAR: return "EEOP_SCAN_SYSVAR";
		case EEOP_WHOLEROW: return "EEOP_WHOLEROW";
		case EEOP_ASSIGN_INNER_VAR: return "EEOP_ASSIGN_INNER_VAR";
		case EEOP_ASSIGN_OUTER_VAR: return "EEOP_ASSIGN_OUTER_VAR";
		case EEOP_ASSIGN_SCAN_VAR: return "EEOP_ASSIGN_SCAN_VAR";
		case EEOP_ASSIGN_TMP: return "EEOP_ASSIGN_TMP";
		case EEOP_ASSIGN_TMP_MAKE_RO: return "EEOP_ASSIGN_TMP_MAKE_RO";
		case EEOP_CONST: return "EEOP_CONST";
		case EEOP_FUNCEXPR: return "EEOP_FUNCEXPR";
		case EEOP_FUNCEXPR_STRICT: return "EEOP_FUNCEXPR_STRICT";
		case EEOP_FUNCEXPR_STRICT_1: return "EEOP_FUNCEXPR_STRICT_1";
		case EEOP_FUNCEXPR_STRICT_2: return "EEOP_FUNCEXPR_STRICT_2";
		case EEOP_FUNCEXPR_FUSAGE: return "EEOP_FUNCEXPR_FUSAGE";
		case EEOP_FUNCEXPR_STRICT_FUSAGE: return "EEOP_FUNCEXPR_STRICT_FUSAGE";
		case EEOP_BOOL_AND_STEP_FIRST: return "EEOP_BOOL_AND_STEP_FIRST";
		case EEOP_BOOL_AND_STEP: return "EEOP_BOOL_AND_STEP";
		case EEOP_BOOL_AND_STEP_LAST: return "EEOP_BOOL_AND_STEP_LAST";
		case EEOP_BOOL_OR_STEP_FIRST: return "EEOP_BOOL_OR_STEP_FIRST";
		case EEOP_BOOL_OR_STEP: return "EEOP_BOOL_OR_STEP";
		case EEOP_BOOL_OR_STEP_LAST: return "EEOP_BOOL_OR_STEP_LAST";
		case EEOP_BOOL_NOT_STEP: return "EEOP_BOOL_NOT_STEP";
		case EEOP_QUAL: return "EEOP_QUAL";
		case EEOP_JUMP: return "EEOP_JUMP";
		case EEOP_JUMP_IF_NULL: return "EEOP_JUMP_IF_NULL";
		case EEOP_JUMP_IF_NOT_NULL: return "EEOP_JUMP_IF_NOT_NULL";
		case EEOP_JUMP_IF_NOT_TRUE: return "EEOP_JUMP_IF_NOT_TRUE";
		case EEOP_NULLTEST_ISNULL: return "EEOP_NULLTEST_ISNULL";
		case EEOP_NULLTEST_ISNOTNULL: return "EEOP_NULLTEST_ISNOTNULL";
		case EEOP_NULLTEST_ROWISNULL: return "EEOP_NULLTEST_ROWISNULL";
		case EEOP_NULLTEST_ROWISNOTNULL: return "EEOP_NULLTEST_ROWISNOTNULL";
		case EEOP_BOOLTEST_IS_TRUE: return "EEOP_BOOLTEST_IS_TRUE";
		case EEOP_BOOLTEST_IS_NOT_TRUE: return "EEOP_BOOLTEST_IS_NOT_TRUE";
		case EEOP_BOOLTEST_IS_FALSE: return "EEOP_BOOLTEST_IS_FALSE";
		case EEOP_BOOLTEST_IS_NOT_FALSE: return "EEOP_BOOLTEST_IS_NOT_FALSE";
		case EEOP_PARAM_EXEC: return "EEOP_PARAM_EXEC";
		case EEOP_PARAM_EXTERN: return "EEOP_PARAM_EXTERN";
		case EEOP_PARAM_CALLBACK: return "EEOP_PARAM_CALLBACK";
		case EEOP_CASE_TESTVAL: return "EEOP_CASE_TESTVAL";
		case EEOP_MAKE_READONLY: return "EEOP_MAKE_READONLY";
		case EEOP_IOCOERCE: return "EEOP_IOCOERCE";
		case EEOP_DISTINCT: return "EEOP_DISTINCT";
		case EEOP_NOT_DISTINCT: return "EEOP_NOT_DISTINCT";
		case EEOP_NULLIF: return "EEOP_NULLIF";
		case EEOP_SQLVALUEFUNCTION: return "EEOP_SQLVALUEFUNCTION";
		case EEOP_CURRENTOFEXPR: return "EEOP_CURRENTOFEXPR";
		case EEOP_NEXTVALUEEXPR: return "EEOP_NEXTVALUEEXPR";
		case EEOP_ARRAYEXPR: return "EEOP_ARRAYEXPR";
		case EEOP_ARRAYCOERCE_RELABEL: return "EEOP_ARRAYCOERCE_RELABEL";
		case EEOP_ARRAYCOERCE_UNPACK: return "EEOP_ARRAYCOERCE_UNPACK";
		case EEOP_ARRAYCOERCE_PACK: return "EEOP_ARRAYCOERCE_PACK";
		case EEOP_ROW: return "EEOP_ROW";
		case EEOP_ROWCOMPARE_STEP: return "EEOP_ROWCOMPARE_STEP";
		case EEOP_ROWCOMPARE_FINAL: return "EEOP_ROWCOMPARE_FINAL";
		case EEOP_MINMAX: return "EEOP_MINMAX";
		case EEOP_FIELDSELECT: return "EEOP_FIELDSELECT";
		case EEOP_FIELDSTORE_DEFORM: return "EEOP_FIELDSTORE_DEFORM";
		case EEOP_FIELDSTORE_FORM: return "EEOP_FIELDSTORE_FORM";
		case EEOP_SBSREF_SUBSCRIPT: return "EEOP_SBSREF_SUBSCRIPT";
		case EEOP_SBSREF_OLD: return "EEOP_SBSREF_OLD";
		case EEOP_SBSREF_ASSIGN: return "EEOP_SBSREF_ASSIGN";
		case EEOP_SBSREF_FETCH: return "EEOP_SBSREF_FETCH";
		case EEOP_DOMAIN_TESTVAL: return "EEOP_DOMAIN_TESTVAL";
		case EEOP_DOMAIN_NOTNULL: return "EEOP_DOMAIN_NOTNULL";
		case EEOP_DOMAIN_CHECK: return "EEOP_DOMAIN_CHECK";
		case EEOP_CONVERT_ROWTYPE: return "EEOP_CONVERT_ROWTYPE";
		case EEOP_SCALARARRAYOP: return "EEOP_SCALARARRAYOP";
		case EEOP_XMLEXPR: return "EEOP_XMLEXPR";
		case EEOP_AGGREF: return "EEOP_AGGREF";
		case EEOP_GROUPING_FUNC: return "EEOP_GROUPING_FUNC";
		case EEOP_WINDOW_FUNC: return "EEOP_WINDOW_FUNC";
		case EEOP_SUBPLAN: return "EEOP_SUBPLAN";
		case EEOP_ALTERNATIVE_SUBPLAN: return "EEOP_ALTERNATIVE_SUBPLAN";
		case EEOP_AGG_STRICT_DESERIALIZE: return "EEOP_AGG_STRICT_DESERIALIZE";
		case EEOP_AGG_DESERIALIZE: return "EEOP_AGG_DESERIALIZE";
		case EEOP_AGG_STRICT_INPUT_CHECK_ARGS: return "EEOP_AGG_STRICT_INPUT_CHECK_ARGS";
		case EEOP_AGG_STRICT_INPUT_CHECK_ARGS_1: return "EEOP_AGG_STRICT_INPUT_CHECK_ARGS_1";
		case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL: return "EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL";
		case EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL: return "EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL";
		case EEOP_AGG_PLAIN_TRANS_BYVAL: return "EEOP_AGG_PLAIN_TRANS_BYVAL";
		case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF: return "EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF";
		case EEOP_AGG_PLAIN_TRANS_STRICT_BYREF: return "EEOP_AGG_PLAIN_TRANS_STRICT_BYREF";
		case EEOP_AGG_PLAIN_TRANS_BYREF: return "EEOP_AGG_PLAIN_TRANS_BYREF";
		case EEOP_AGG_ORDERED_TRANS_DATUM: return "EEOP_AGG_ORDERED_TRANS_DATUM";
		case EEOP_AGG_ORDERED_TRANS_TUPLE: return "EEOP_AGG_ORDERED_TRANS_TUPLE";
		case EEOP_LAST: return "EEOP_LAST";
	}
	pg_unreachable();
	return NULL;
}
