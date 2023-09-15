/*-------------------------------------------------------------------------
 *
 * execExprInterp.c
 *	  Interpreted evaluation of an expression step list.
 *
 * This file provides either a "direct threaded" (for gcc, clang and
 * compatible) or a "switch threaded" (for all compilers) implementation of
 * expression evaluation.  The former is amongst the fastest known methods
 * of interpreting programs without resorting to assembly level work, or
 * just-in-time compilation, but it requires support for computed gotos.
 * The latter is amongst the fastest approaches doable in standard C.
 *
 * In either case we use ExprEvalStep->opcode to dispatch to the code block
 * within ExecInterpExpr() that implements the specific opcode type.
 *
 * Switch-threading uses a plain switch() statement to perform the
 * dispatch.  This has the advantages of being plain C and allowing the
 * compiler to warn if implementation of a specific opcode has been forgotten.
 * The disadvantage is that dispatches will, as commonly implemented by
 * compilers, happen from a single location, requiring more jumps and causing
 * bad branch prediction.
 *
 * In direct threading, we use gcc's label-as-values extension - also adopted
 * by some other compilers - to replace ExprEvalStep->opcode with the address
 * of the block implementing the instruction. Dispatch to the next instruction
 * is done by a "computed goto".  This allows for better branch prediction
 * (as the jumps are happening from different locations) and fewer jumps
 * (as no preparatory jump to a common dispatch location is needed).
 *
 * When using direct threading, ExecReadyInterpretedExpr will replace
 * each step's opcode field with the address of the relevant code block and
 * ExprState->flags will contain EEO_FLAG_DIRECT_THREADED to remember that
 * that's been done.
 *
 * For very simple instructions the overhead of the full interpreter
 * "startup", as minimal as it is, is noticeable.  Therefore
 * ExecReadyInterpretedExpr will choose to implement certain simple
 * opcode patterns using special fast-path routines (ExecJust*).
 *
 * Complex or uncommon instructions are not implemented in-line in
 * ExecInterpExpr(), rather we call out to a helper function appearing later
 * in this file.  For one reason, there'd not be a noticeable performance
 * benefit, but more importantly those complex routines are intended to be
 * shared between different expression evaluation approaches.  For instance
 * a JIT compiler would generate calls to them.  (This is why they are
 * exported rather than being "static" in this file.)
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/execExprInterp.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heaptoast.h"
#include "catalog/pg_type.h"
#include "commands/sequence.h"
#include "executor/nodeAgg.h"
#include "executor/execExpr.h"
#include "executor/nodeSubplan.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "pgstat.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/expandedrecord.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/jsonfuncs.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"
#include "utils/xml.h"

/*
 * Use computed-goto-based opcode dispatch when computed gotos are available.
 * But use a separate symbol so that it's easy to adjust locally in this file
 * for development and testing.
 */
#ifdef HAVE_COMPUTED_GOTO
#define EEO_USE_COMPUTED_GOTO
#endif							/* HAVE_COMPUTED_GOTO */

/*
 * Macros for opcode dispatch.
 *
 * EEO_SWITCH - just hides the switch if not in use.
 * EEO_CASE - labels the implementation of named expression step type.
 * EEO_DISPATCH - jump to the implementation of the step type for 'op'.
 * EEO_OPCODE - compute opcode required by used expression evaluation method.
 * EEO_NEXT - increment 'op' and jump to correct next step type.
 * EEO_JUMP - jump to the specified step number within the current expression.
 */
#if defined(EEO_USE_COMPUTED_GOTO)

/* struct for jump target -> opcode lookup table */
typedef struct ExprEvalOpLookup
{
	const void *opcode;
	ExprEvalOp	op;
} ExprEvalOpLookup;

/* to make dispatch_table accessible outside ExecInterpExpr() */
static const void **dispatch_table = NULL;

/* jump target -> opcode lookup table */
static ExprEvalOpLookup reverse_dispatch_table[EEOP_LAST];

#define EEO_SWITCH()
#define EEO_CASE(name)		CASE_##name:
#define EEO_DISPATCH()		goto *((void *) op->opcode)
#define EEO_OPCODE(opcode)	((intptr_t) dispatch_table[opcode])

#else							/* !EEO_USE_COMPUTED_GOTO */

#define EEO_SWITCH()		starteval: switch ((ExprEvalOp) op->opcode)
#define EEO_CASE(name)		case name:
#define EEO_DISPATCH()		goto starteval
#define EEO_OPCODE(opcode)	(opcode)

#endif							/* EEO_USE_COMPUTED_GOTO */

#define EEO_NEXT() \
	do { \
		nonconst_op++; \
		op = nonconst_op; \
		EEO_DISPATCH(); \
	} while (0)

#define EEO_JUMP(stepno) \
	do { \
		nonconst_op = &state->steps[stepno]; \
		op = nonconst_op; \
		EEO_DISPATCH(); \
	} while (0)


#define RELPTR_RESOLVE(data, relptr) \
	(void *) (AssertMacro(relptr.ptr.allocno != 0), \
			  (data + relptr.ptr.off))

#define RELPTR_RESOLVE_MAYNULL(data, relptr) \
	(void *) (relptr.ptr.allocno == 0 ? NULL : \
			  (data + relptr.ptr.off))


static Datum ExecInterpExpr(ExprState *state, ExprContext *econtext, bool *isnull);
static void ExecInitInterpreter(void);

/* support functions */
static void CheckVarSlotCompatibility(TupleTableSlot *slot, int attnum, Oid vartype);
static void CheckOpSlotCompatibility(const ExprEvalStep *op, TupleTableSlot *slot);
static TupleDesc get_cached_rowtype(Oid type_id, int32 typmod,
									ExprEvalRowtypeCache *rowcache,
									bool *changed);
static void ExecEvalRowNullInt(ExprState *state, ExprEvalStep *op,
							   ExprContext *econtext, bool checkisnull,
							   NullableDatum *opres);

/* fast-path evaluation functions */
static Datum ExecJustInnerVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustOuterVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustScanVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignInnerVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignOuterVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignScanVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustConst(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustInnerVarVirt(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustOuterVarVirt(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustScanVarVirt(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignInnerVarVirt(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignOuterVarVirt(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignScanVarVirt(ExprState *state, ExprContext *econtext, bool *isnull);

/* execution helper functions */
static pg_attribute_always_inline void ExecAggPlainTransByVal(AggState *aggstate,
															  const AggStatePerCallContext *percall,
															  AggStatePerGroup pergroup,
															  FunctionCallInfo fcinfo,
															  PGFunction fn_addr);
static pg_attribute_always_inline void ExecAggPlainTransByRef(AggState *aggstate,
															  const AggStatePerCallContext *percall,
															  AggStatePerGroup pergroup,
															  FunctionCallInfo fcinfo,
															  PGFunction fn_addr);
/*
 * ScalarArrayOpExprHashEntry
 * 		Hash table entry type used during EEOP_HASHED_SCALARARRAYOP
 */
typedef struct ScalarArrayOpExprHashEntry
{
	Datum		key;
	uint32		status;			/* hash status */
	uint32		hash;			/* hash value (cached) */
} ScalarArrayOpExprHashEntry;

#define SH_PREFIX saophash
#define SH_ELEMENT_TYPE ScalarArrayOpExprHashEntry
#define SH_KEY_TYPE Datum
#define SH_SCOPE static inline
#define SH_DECLARE
#include "lib/simplehash.h"

static bool saop_hash_element_match(struct saophash_hash *tb, Datum key1,
									Datum key2);
static uint32 saop_element_hash(struct saophash_hash *tb, Datum key);

/*
 * ScalarArrayOpExprHashTable
 *		Hash table for EEOP_HASHED_SCALARARRAYOP
 */
typedef struct ScalarArrayOpExprHashTable
{
	saophash_hash *hashtab;		/* underlying hash table */
	FmgrInfo	hash_finfo;		/* function's lookup data */

	PGFunction match_fn_addr;
	FunctionCallInfo match_fcinfo;
	FunctionCallInfoBaseData hash_fcinfo_data;	/* arguments etc */
} ScalarArrayOpExprHashTable;

/* Define parameters for ScalarArrayOpExpr hash table code generation. */
#define SH_PREFIX saophash
#define SH_ELEMENT_TYPE ScalarArrayOpExprHashEntry
#define SH_KEY_TYPE Datum
#define SH_KEY key
#define SH_HASH_KEY(tb, key) saop_element_hash(tb, key)
#define SH_EQUAL(tb, a, b) saop_hash_element_match(tb, a, b)
#define SH_SCOPE static inline
#define SH_STORE_HASH
#define SH_GET_HASH(tb, a) a->hash
#define SH_DEFINE
#include "lib/simplehash.h"

/*
 * Prepare ExprState for interpreted execution.
 */
void
ExecReadyInterpretedExpr(ExprState *state, ExprStateBuilder *esb)
{
	/* Ensure one-time interpreter setup has been done */
	ExecInitInterpreter();

	/* Simple validity checks on expression */
	Assert(state->steps_final_len >= 1);
	Assert(state->steps[esb->steps_len - 1].opcode == EEOP_DONE_RETURN ||
		   state->steps[esb->steps_len - 1].opcode == EEOP_DONE_NO_RETURN);

	/*
	 * Don't perform redundant initialization. This is unreachable in current
	 * cases, but might be hit if there's additional expression evaluation
	 * methods that rely on interpreted execution to work.
	 */
	if (esb->flags & EEO_FLAG_INTERPRETER_INITIALIZED)
		return;

	/*
	 * First time through, check whether attribute matches Var.  Might not be
	 * ok anymore, due to schema changes. We do that by setting up a callback
	 * that does checking on the first call, which then sets the evalfunc
	 * callback to the actual method of execution.
	 */
	state->evalfunc = ExecInterpExprStillValid;

	/* DIRECT_THREADED should not already be set */
	Assert((esb->flags & EEO_FLAG_DIRECT_THREADED) == 0);

	/*
	 * There shouldn't be any errors before the expression is fully
	 * initialized, and even if so, it'd lead to the expression being
	 * abandoned.  So we can set the flag now and save some code.
	 */
	state->flags |= EEO_FLAG_INTERPRETER_INITIALIZED;

	/*
	 * Select fast-path evalfuncs for very simple expressions.  "Starting up"
	 * the full interpreter is a measurable overhead for these, and these
	 * patterns occur often enough to be worth optimizing.
	 */
	if (state->steps_final_len == 3)
	{
		ExprEvalOp	step0 = state->steps[0].opcode;
		ExprEvalOp	step1 = state->steps[1].opcode;

		if (step0 == EEOP_INNER_FETCHSOME &&
			step1 == EEOP_INNER_VAR)
		{
			state->evalfunc_private = (void *) ExecJustInnerVar;
			return;
		}
		else if (step0 == EEOP_OUTER_FETCHSOME &&
				 step1 == EEOP_OUTER_VAR)
		{
			state->evalfunc_private = (void *) ExecJustOuterVar;
			return;
		}
		else if (step0 == EEOP_SCAN_FETCHSOME &&
				 step1 == EEOP_SCAN_VAR)
		{
			state->evalfunc_private = (void *) ExecJustScanVar;
			return;
		}
		else if (step0 == EEOP_INNER_FETCHSOME &&
				 step1 == EEOP_ASSIGN_INNER_VAR)
		{
			state->evalfunc_private = (void *) ExecJustAssignInnerVar;
			return;
		}
		else if (step0 == EEOP_OUTER_FETCHSOME &&
				 step1 == EEOP_ASSIGN_OUTER_VAR)
		{
			state->evalfunc_private = (void *) ExecJustAssignOuterVar;
			return;
		}
		else if (step0 == EEOP_SCAN_FETCHSOME &&
				 step1 == EEOP_ASSIGN_SCAN_VAR)
		{
			state->evalfunc_private = (void *) ExecJustAssignScanVar;
			return;
		}
	}
	else if (state->steps_final_len == 2)
	{
		ExprEvalOp	step0 = state->steps[0].opcode;

		if (step0 == EEOP_CONST)
		{
			state->evalfunc_private = (void *) ExecJustConst;
			return;
		}
		else if (step0 == EEOP_INNER_VAR)
		{
			state->evalfunc_private = (void *) ExecJustInnerVarVirt;
			return;
		}
		else if (step0 == EEOP_OUTER_VAR)
		{
			state->evalfunc_private = (void *) ExecJustOuterVarVirt;
			return;
		}
		else if (step0 == EEOP_SCAN_VAR)
		{
			state->evalfunc_private = (void *) ExecJustScanVarVirt;
			return;
		}
		else if (step0 == EEOP_ASSIGN_INNER_VAR)
		{
			state->evalfunc_private = (void *) ExecJustAssignInnerVarVirt;
			return;
		}
		else if (step0 == EEOP_ASSIGN_OUTER_VAR)
		{
			state->evalfunc_private = (void *) ExecJustAssignOuterVarVirt;
			return;
		}
		else if (step0 == EEOP_ASSIGN_SCAN_VAR)
		{
			state->evalfunc_private = (void *) ExecJustAssignScanVarVirt;
			return;
		}
	}

#if defined(EEO_USE_COMPUTED_GOTO)

	/*
	 * In the direct-threaded implementation, replace each opcode with the
	 * address to jump to.  (Use ExecEvalStepOp() to get back the opcode.)
	 */
	for (int off = 0; off < state->steps_final_len; off++)
	{
		ExprEvalStep *op = &state->steps[off];

		op->opcode = EEO_OPCODE(op->opcode);
	}

	state->flags |= EEO_FLAG_DIRECT_THREADED;
#endif							/* EEO_USE_COMPUTED_GOTO */

	state->evalfunc_private = (void *) ExecInterpExpr;
}


/*
 * Evaluate expression identified by "state" in the execution context
 * given by "econtext".  *isnull is set to the is-null flag for the result,
 * and the Datum value is the function result.
 *
 * As a special case, return the dispatch table's address if state is NULL.
 * This is used by ExecInitInterpreter to set up the dispatch_table global.
 * (Only applies when EEO_USE_COMPUTED_GOTO is defined.)
 */
static Datum
ExecInterpExpr(ExprState *state, ExprContext *econtext, bool *isnull)
{
	const ExprEvalStep *op;
	ExprEvalStep *nonconst_op;
	TupleTableSlot *resultslot;
	TupleTableSlot *innerslot;
	TupleTableSlot *outerslot;
	TupleTableSlot *scanslot;
	char *const data = state ?
		(char *) state + state->mutable_off : NULL;

	/*
	 * This array has to be in the same order as enum ExprEvalOp.
	 */
#if defined(EEO_USE_COMPUTED_GOTO)
	static const void *const dispatch_table[] = {
		&&CASE_EEOP_DONE_RETURN,
		&&CASE_EEOP_DONE_NO_RETURN,
		&&CASE_EEOP_INNER_FETCHSOME,
		&&CASE_EEOP_OUTER_FETCHSOME,
		&&CASE_EEOP_SCAN_FETCHSOME,
		&&CASE_EEOP_INNER_VAR,
		&&CASE_EEOP_OUTER_VAR,
		&&CASE_EEOP_SCAN_VAR,
		&&CASE_EEOP_INNER_SYSVAR,
		&&CASE_EEOP_OUTER_SYSVAR,
		&&CASE_EEOP_SCAN_SYSVAR,
		&&CASE_EEOP_WHOLEROW,
		&&CASE_EEOP_ASSIGN_INNER_VAR,
		&&CASE_EEOP_ASSIGN_OUTER_VAR,
		&&CASE_EEOP_ASSIGN_SCAN_VAR,
		&&CASE_EEOP_ASSIGN_TMP,
		&&CASE_EEOP_ASSIGN_TMP_MAKE_RO,
		&&CASE_EEOP_CONST,
		&&CASE_EEOP_FUNCEXPR,
		&&CASE_EEOP_FUNCEXPR_STRICT,
		&&CASE_EEOP_FUNCEXPR_STRICT_1,
		&&CASE_EEOP_FUNCEXPR_STRICT_2,
		&&CASE_EEOP_FUNCEXPR_FUSAGE,
		&&CASE_EEOP_FUNCEXPR_STRICT_FUSAGE,
		&&CASE_EEOP_BOOL_AND_STEP_FIRST,
		&&CASE_EEOP_BOOL_AND_STEP,
		&&CASE_EEOP_BOOL_AND_STEP_LAST,
		&&CASE_EEOP_BOOL_OR_STEP_FIRST,
		&&CASE_EEOP_BOOL_OR_STEP,
		&&CASE_EEOP_BOOL_OR_STEP_LAST,
		&&CASE_EEOP_BOOL_NOT_STEP,
		&&CASE_EEOP_QUAL,
		&&CASE_EEOP_JUMP,
		&&CASE_EEOP_JUMP_IF_NULL,
		&&CASE_EEOP_JUMP_IF_NOT_NULL,
		&&CASE_EEOP_JUMP_IF_NOT_TRUE,
		&&CASE_EEOP_NULLTEST_ISNULL,
		&&CASE_EEOP_NULLTEST_ISNOTNULL,
		&&CASE_EEOP_NULLTEST_ROWISNULL,
		&&CASE_EEOP_NULLTEST_ROWISNOTNULL,
		&&CASE_EEOP_BOOLTEST_IS_TRUE,
		&&CASE_EEOP_BOOLTEST_IS_NOT_TRUE,
		&&CASE_EEOP_BOOLTEST_IS_FALSE,
		&&CASE_EEOP_BOOLTEST_IS_NOT_FALSE,
		&&CASE_EEOP_PARAM_EXEC,
		&&CASE_EEOP_PARAM_EXTERN,
		&&CASE_EEOP_PARAM_CALLBACK,
		&&CASE_EEOP_CASE_TESTVAL,
		&&CASE_EEOP_MAKE_READONLY,
		&&CASE_EEOP_IOCOERCE,
		&&CASE_EEOP_DISTINCT,
		&&CASE_EEOP_NOT_DISTINCT,
		&&CASE_EEOP_NULLIF,
		&&CASE_EEOP_SQLVALUEFUNCTION,
		&&CASE_EEOP_CURRENTOFEXPR,
		&&CASE_EEOP_NEXTVALUEEXPR,
		&&CASE_EEOP_ARRAYEXPR,
		&&CASE_EEOP_ARRAYCOERCE_RELABEL,
		&&CASE_EEOP_ARRAYCOERCE_UNPACK,
		&&CASE_EEOP_ARRAYCOERCE_PACK,
		&&CASE_EEOP_ROW,
		&&CASE_EEOP_ROWCOMPARE_STEP,
		&&CASE_EEOP_ROWCOMPARE_FINAL,
		&&CASE_EEOP_MINMAX,
		&&CASE_EEOP_FIELDSELECT,
		&&CASE_EEOP_FIELDSTORE_DEFORM,
		&&CASE_EEOP_FIELDSTORE_FORM,
		&&CASE_EEOP_SBSREF_SUBSCRIPTS,
		&&CASE_EEOP_SBSREF_OLD,
		&&CASE_EEOP_SBSREF_ASSIGN,
		&&CASE_EEOP_SBSREF_FETCH,
		&&CASE_EEOP_DOMAIN_TESTVAL,
		&&CASE_EEOP_DOMAIN_NOTNULL,
		&&CASE_EEOP_DOMAIN_CHECK,
		&&CASE_EEOP_CONVERT_ROWTYPE,
		&&CASE_EEOP_SCALARARRAYOP,
		&&CASE_EEOP_HASHED_SCALARARRAYOP,
		&&CASE_EEOP_XMLEXPR,
		&&CASE_EEOP_JSON_CONSTRUCTOR,
		&&CASE_EEOP_IS_JSON,
		&&CASE_EEOP_AGGREF,
		&&CASE_EEOP_GROUPING_FUNC,
		&&CASE_EEOP_WINDOW_FUNC,
		&&CASE_EEOP_SUBPLAN,
		&&CASE_EEOP_AGG_STRICT_DESERIALIZE,
		&&CASE_EEOP_AGG_DESERIALIZE,
		&&CASE_EEOP_AGG_STRICT_INPUT_CHECK_ARGS,
		&&CASE_EEOP_AGG_STRICT_INPUT_CHECK_ARGS_1,
		&&CASE_EEOP_AGG_PLAIN_PERGROUP_NULLCHECK,
		&&CASE_EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL,
		&&CASE_EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL,
		&&CASE_EEOP_AGG_PLAIN_TRANS_BYVAL,
		&&CASE_EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF,
		&&CASE_EEOP_AGG_PLAIN_TRANS_STRICT_BYREF,
		&&CASE_EEOP_AGG_PLAIN_TRANS_BYREF,
		&&CASE_EEOP_AGG_PRESORTED_DISTINCT_SINGLE,
		&&CASE_EEOP_AGG_PRESORTED_DISTINCT_MULTI,
		&&CASE_EEOP_AGG_ORDERED_TRANS_DATUM,
		&&CASE_EEOP_AGG_ORDERED_TRANS_TUPLE,
		&&CASE_EEOP_LAST
	};

	StaticAssertDecl(lengthof(dispatch_table) == EEOP_LAST + 1,
					 "dispatch_table out of whack with ExprEvalOp");

	if (unlikely(state == NULL))
		return PointerGetDatum(dispatch_table);
#else
	Assert(state != NULL);
#endif							/* EEO_USE_COMPUTED_GOTO */

	/* setup state */
	op = nonconst_op = state->steps;
	resultslot = state->resultslot;
	innerslot = econtext->ecxt_innertuple;
	outerslot = econtext->ecxt_outertuple;
	scanslot = econtext->ecxt_scantuple;

#if defined(EEO_USE_COMPUTED_GOTO)
	EEO_DISPATCH();
#endif

	EEO_SWITCH()
	{
		EEO_CASE(EEOP_DONE_RETURN)
		{
			NullableDatum *result;

			result = RELPTR_RESOLVE(data, op->result);
			*isnull = result->isnull;
			return result->value;
		}

		EEO_CASE(EEOP_DONE_NO_RETURN)
		{
			Assert(isnull == NULL);
			return 0;
		}

		EEO_CASE(EEOP_INNER_FETCHSOME)
		{
			CheckOpSlotCompatibility(op, innerslot);

			slot_getsomeattrs(innerslot, op->d.fetch.last_var);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_OUTER_FETCHSOME)
		{
			CheckOpSlotCompatibility(op, outerslot);

			slot_getsomeattrs(outerslot, op->d.fetch.last_var);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SCAN_FETCHSOME)
		{
			CheckOpSlotCompatibility(op, scanslot);

			slot_getsomeattrs(scanslot, op->d.fetch.last_var);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_INNER_VAR)
		{
			int			attnum = op->d.var.attnum;
			NullableDatum *opres = RELPTR_RESOLVE(data, op->result);

			/*
			 * Since we already extracted all referenced columns from the
			 * tuple with a FETCHSOME step, we can just grab the value
			 * directly out of the slot's decomposed-data arrays.  But let's
			 * have an Assert to check that that did happen.
			 */
			Assert(attnum >= 0 && attnum < innerslot->tts_nvalid);
			*opres = innerslot->tts_values[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_OUTER_VAR)
		{
			int			attnum = op->d.var.attnum;
			NullableDatum *opres = RELPTR_RESOLVE(data, op->result);

			/* See EEOP_INNER_VAR comments */
			Assert(attnum >= 0 && attnum < outerslot->tts_nvalid);
			*opres = outerslot->tts_values[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SCAN_VAR)
		{
			int			attnum = op->d.var.attnum;
			NullableDatum *opres = RELPTR_RESOLVE(data, op->result);

			/* See EEOP_INNER_VAR comments */

			Assert(attnum >= 0 && attnum < scanslot->tts_nvalid);
			*opres = scanslot->tts_values[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_INNER_SYSVAR)
		{
			ExecEvalSysVar(state, op->d.var.attnum, innerslot,
						   RELPTR_RESOLVE(data, op->result));
			EEO_NEXT();
		}

		EEO_CASE(EEOP_OUTER_SYSVAR)
		{
			ExecEvalSysVar(state, op->d.var.attnum, outerslot,
						   RELPTR_RESOLVE(data, op->result));
			EEO_NEXT();
		}

		EEO_CASE(EEOP_SCAN_SYSVAR)
		{
			ExecEvalSysVar(state, op->d.var.attnum, scanslot,
						   RELPTR_RESOLVE(data, op->result));
			EEO_NEXT();
		}

		EEO_CASE(EEOP_WHOLEROW)
		{
			/* too complex for an inline implementation */
			ExecEvalWholeRowVar(state, nonconst_op, econtext,
								RELPTR_RESOLVE(data, op->result));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_INNER_VAR)
		{
			int			resultnum = op->d.assign_var.resultnum;
			int			attnum = op->d.assign_var.attnum;

			/*
			 * We do not need CheckVarSlotCompatibility here; that was taken
			 * care of at compilation time.  But see EEOP_INNER_VAR comments.
			 */
			Assert(attnum >= 0 && attnum < innerslot->tts_nvalid);
			Assert(resultnum >= 0 && resultnum < resultslot->tts_tupleDescriptor->natts);
			resultslot->tts_values[resultnum] = innerslot->tts_values[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_OUTER_VAR)
		{
			int			resultnum = op->d.assign_var.resultnum;
			int			attnum = op->d.assign_var.attnum;

			/*
			 * We do not need CheckVarSlotCompatibility here; that was taken
			 * care of at compilation time.  But see EEOP_INNER_VAR comments.
			 */
			Assert(attnum >= 0 && attnum < outerslot->tts_nvalid);
			Assert(resultnum >= 0 && resultnum < resultslot->tts_tupleDescriptor->natts);
			resultslot->tts_values[resultnum] = outerslot->tts_values[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_SCAN_VAR)
		{
			int			resultnum = op->d.assign_var.resultnum;
			int			attnum = op->d.assign_var.attnum;

			/*
			 * We do not need CheckVarSlotCompatibility here; that was taken
			 * care of at compilation time.  But see EEOP_INNER_VAR comments.
			 */
			Assert(attnum >= 0 && attnum < scanslot->tts_nvalid);
			Assert(resultnum >= 0 && resultnum < resultslot->tts_tupleDescriptor->natts);
			resultslot->tts_values[resultnum] = scanslot->tts_values[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_TMP)
		{
			int			resultnum = op->d.assign_tmp.resultnum;
			NullableDatum *result;

			Assert(resultnum >= 0 && resultnum < resultslot->tts_tupleDescriptor->natts);

			result = RELPTR_RESOLVE(data, op->result);
			resultslot->tts_values[resultnum] = *result;
			resultslot->tts_values[resultnum] = *result;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_TMP_MAKE_RO)
		{
			int			resultnum = op->d.assign_tmp.resultnum;
			NullableDatum *rsval = &resultslot->tts_values[resultnum];
			NullableDatum *result;

			Assert(resultnum >= 0 && resultnum < resultslot->tts_tupleDescriptor->natts);

			result = RELPTR_RESOLVE(data, op->result);
			rsval->isnull = result->isnull;
			if (!rsval->isnull)
				rsval->value =
					MakeExpandedObjectReadOnlyInternal(result->value);
			else
				rsval->value = result->value;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CONST)
		{
			NullableDatum *opres = RELPTR_RESOLVE(data, op->result);

			*opres = op->d.constval.value;

			EEO_NEXT();
		}

		/*
		 * Function-call implementations. Arguments have previously been
		 * evaluated directly into fcinfo->args.
		 *
		 * As both STRICT checks and function-usage are noticeable performance
		 * wise, and function calls are a very hot-path (they also back
		 * operators!), it's worth having so many separate opcodes.
		 *
		 * Note: the reason for using a temporary variable "d", here and in
		 * other places, is that some compilers think "op->result->value = f();"
		 * requires them to evaluate op->result into a register before calling
		 * f(), just in case f() is able to modify op->result somehow.  The
		 * extra line of code can save a useless register spill and reload
		 * across the function call.
		 */
		EEO_CASE(EEOP_FUNCEXPR)
		{
			FunctionCallInfo fcinfo =
				RELPTR_RESOLVE(data, op->d.func.fcinfo_data);
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);
			Datum		d;

			fcinfo->isnull = false;
			d = op->d.func.fn_addr(fcinfo);
			opres->value = d;
			opres->isnull = fcinfo->isnull;

			EEO_NEXT();
		}

		/* strict function call with more than two arguments */
		EEO_CASE(EEOP_FUNCEXPR_STRICT)
		{
			FunctionCallInfo fcinfo =
				RELPTR_RESOLVE(data, op->d.func.fcinfo_data);
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);
			NullableDatum *args = fcinfo->args;
			int			nargs = op->d.func.nargs;
			Datum		d;

			Assert(nargs > 2);

			/* strict function, so check for NULL args */
			for (int argno = 0; argno < nargs; argno++)
			{
				if (args[argno].isnull)
				{
					opres->isnull = true;
					goto strictfail;
				}
			}
			fcinfo->isnull = false;
			d = op->d.func.fn_addr(fcinfo);
			opres->value = d;
			opres->isnull = fcinfo->isnull;

	strictfail:
			EEO_NEXT();
		}

		/* strict function call with one argument */
		EEO_CASE(EEOP_FUNCEXPR_STRICT_1)
		{
			FunctionCallInfo fcinfo =
				RELPTR_RESOLVE(data, op->d.func.fcinfo_data);
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);
			NullableDatum *args = fcinfo->args;

			Assert(op->d.func.nargs == 1);

			/* strict function, so check for NULL args */
			if (args[0].isnull)
				opres->isnull = true;
			else
			{
				Datum		d;

				fcinfo->isnull = false;
				d = op->d.func.fn_addr(fcinfo);
				opres->value = d;
				opres->isnull = fcinfo->isnull;
			}

			EEO_NEXT();
		}

		/* strict function call with two arguments */
		EEO_CASE(EEOP_FUNCEXPR_STRICT_2)
		{
			FunctionCallInfo fcinfo =
				RELPTR_RESOLVE(data, op->d.func.fcinfo_data);
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);
			NullableDatum *args = fcinfo->args;

			Assert(op->d.func.nargs == 2);

			/* strict function, so check for NULL args */
			if (args[0].isnull || args[1].isnull)
				opres->isnull = true;
			else
			{
				Datum		d;

				fcinfo->isnull = false;
				d = op->d.func.fn_addr(fcinfo);
				opres->value = d;
				opres->isnull = fcinfo->isnull;
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_FUNCEXPR_FUSAGE)
		{
			/* not common enough to inline */
			ExecEvalFuncExprFusage(state, op,
								   RELPTR_RESOLVE(data, op->result),
								   RELPTR_RESOLVE(data, op->d.func.fcinfo_data));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_FUNCEXPR_STRICT_FUSAGE)
		{
			/* not common enough to inline */
			ExecEvalFuncExprStrictFusage(state, op,
										 RELPTR_RESOLVE(data, op->result),
										 RELPTR_RESOLVE(data, op->d.func.fcinfo_data));

			EEO_NEXT();
		}

		/*
		 * If any of its clauses is FALSE, an AND's result is FALSE regardless
		 * of the states of the rest of the clauses, so we can stop evaluating
		 * and return FALSE immediately.  If none are FALSE and one or more is
		 * NULL, we return NULL; otherwise we return TRUE.  This makes sense
		 * when you interpret NULL as "don't know": perhaps one of the "don't
		 * knows" would have been FALSE if we'd known its value.  Only when
		 * all the inputs are known to be TRUE can we state confidently that
		 * the AND's result is TRUE.
		 */
		EEO_CASE(EEOP_BOOL_AND_STEP_FIRST)
		{
			bool *anynull =
				RELPTR_RESOLVE(data, op->d.boolexpr.anynull);

			*anynull = false;

			/*
			 * EEOP_BOOL_AND_STEP_FIRST resets anynull, otherwise it's the
			 * same as EEOP_BOOL_AND_STEP - so fall through to that.
			 */

			/* FALL THROUGH */
		}

		EEO_CASE(EEOP_BOOL_AND_STEP)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);

			if (opres->isnull)
			{
				bool *anynull =
					RELPTR_RESOLVE(data, op->d.boolexpr.anynull);

				*anynull = true;
			}
			else if (!DatumGetBool(opres->value))
			{
				/* result is already set to FALSE, need not change it */
				/* bail out early */
				EEO_JUMP(op->d.boolexpr.jumpdone);
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOL_AND_STEP_LAST)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);
			bool *anynull =
				RELPTR_RESOLVE(data, op->d.boolexpr.anynull);

			if (opres->isnull)
			{
				/* result is already set to NULL, need not change it */
			}
			else if (!DatumGetBool(opres->value))
			{
				/* result is already set to FALSE, need not change it */

				/*
				 * No point jumping early to jumpdone - would be same target
				 * (as this is the last argument to the AND expression),
				 * except more expensive.
				 */
			}
			else if (*anynull)
				*opres = NULL_DATUM;
			else
			{
				/* result is already set to TRUE, need not change it */
			}

			EEO_NEXT();
		}

		/*
		 * If any of its clauses is TRUE, an OR's result is TRUE regardless of
		 * the states of the rest of the clauses, so we can stop evaluating
		 * and return TRUE immediately.  If none are TRUE and one or more is
		 * NULL, we return NULL; otherwise we return FALSE.  This makes sense
		 * when you interpret NULL as "don't know": perhaps one of the "don't
		 * knows" would have been TRUE if we'd known its value.  Only when all
		 * the inputs are known to be FALSE can we state confidently that the
		 * OR's result is FALSE.
		 */
		EEO_CASE(EEOP_BOOL_OR_STEP_FIRST)
		{
			bool *anynull =
				RELPTR_RESOLVE(data, op->d.boolexpr.anynull);

			*anynull = false;

			/*
			 * EEOP_BOOL_OR_STEP_FIRST resets anynull, otherwise it's the same
			 * as EEOP_BOOL_OR_STEP - so fall through to that.
			 */

			/* FALL THROUGH */
		}

		EEO_CASE(EEOP_BOOL_OR_STEP)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);
			bool *anynull =
				RELPTR_RESOLVE(data, op->d.boolexpr.anynull);

			if (opres->isnull)
			{
				*anynull = true;
			}
			else if (DatumGetBool(opres->value))
			{
				/* result is already set to TRUE, need not change it */
				/* bail out early */
				EEO_JUMP(op->d.boolexpr.jumpdone);
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOL_OR_STEP_LAST)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);
			bool *anynull =
				RELPTR_RESOLVE(data, op->d.boolexpr.anynull);

			if (opres->isnull)
			{
				/* result is already set to NULL, need not change it */
			}
			else if (DatumGetBool(opres->value))
			{
				/* result is already set to TRUE, need not change it */

				/*
				 * No point jumping to jumpdone - would be same target (as
				 * this is the last argument to the AND expression), except
				 * more expensive.
				 */
			}
			else if (*anynull)
				*opres = NULL_DATUM;
			else
			{
				/* result is already set to FALSE, need not change it */
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOL_NOT_STEP)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);

			/*
			 * Evaluation of 'not' is simple... if expr is false, then return
			 * 'true' and vice versa.  It's safe to do this even on a
			 * nominally null value, so we ignore result->isnull; that means
			 * that NULL in produces NULL out, which is what we want.
			 */
			opres->value = BoolGetDatum(!DatumGetBool(opres->value));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_QUAL)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);

			/* simplified version of BOOL_AND_STEP for use by ExecQual() */

			/* If argument (also result) is false or null ... */
			if (opres->isnull ||
				!DatumGetBool(opres->value))
			{
				/* ... bail out early, returning FALSE */
				opres->isnull = false;
				opres->value = BoolGetDatum(false);
				EEO_JUMP(op->d.qualexpr.jumpdone);
			}

			/*
			 * Otherwise, leave the TRUE value in place, in case this is the
			 * last qual.  Then, TRUE is the correct answer.
			 */

			EEO_NEXT();
		}

		EEO_CASE(EEOP_JUMP)
		{
			/* Unconditionally jump to target step */
			EEO_JUMP(op->d.jump.jumpdone);
		}

		EEO_CASE(EEOP_JUMP_IF_NULL)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);

			/* Transfer control if current result is null */
			if (opres->isnull)
				EEO_JUMP(op->d.jump.jumpdone);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_JUMP_IF_NOT_NULL)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);

			/* Transfer control if current result is non-null */
			if (!opres->isnull)
				EEO_JUMP(op->d.jump.jumpdone);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_JUMP_IF_NOT_TRUE)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);

			/* Transfer control if current result is null or false */
			if (opres->isnull || !DatumGetBool(opres->value))
				EEO_JUMP(op->d.jump.jumpdone);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NULLTEST_ISNULL)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);

			opres->value = BoolGetDatum(opres->isnull);
			opres->isnull = false;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NULLTEST_ISNOTNULL)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);

			opres->value = BoolGetDatum(!opres->isnull);
			opres->isnull = false;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NULLTEST_ROWISNULL)
		{
			/* out of line implementation: too large */
			ExecEvalRowNull(state, nonconst_op, econtext,
							RELPTR_RESOLVE(data, op->result));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NULLTEST_ROWISNOTNULL)
		{
			/* out of line implementation: too large */
			ExecEvalRowNotNull(state, nonconst_op, econtext,
							   RELPTR_RESOLVE(data, op->result));

			EEO_NEXT();
		}

		/* BooleanTest implementations for all booltesttypes */

		EEO_CASE(EEOP_BOOLTEST_IS_TRUE)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);

			if (opres->isnull)
			{
				opres->value = BoolGetDatum(false);
				opres->isnull = false;
			}
			/* else, input value is the correct output as well */

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOLTEST_IS_NOT_TRUE)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);

			if (opres->isnull)
			{
				opres->value = BoolGetDatum(true);
				opres->isnull = false;
			}
			else
				opres->value = BoolGetDatum(!DatumGetBool(opres->value));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOLTEST_IS_FALSE)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);

			if (opres->isnull)
			{
				opres->value = BoolGetDatum(false);
				opres->isnull = false;
			}
			else
				opres->value = BoolGetDatum(!DatumGetBool(opres->value));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOLTEST_IS_NOT_FALSE)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);

			if (opres->isnull)
			{
				opres->value = BoolGetDatum(true);
				opres->isnull = false;
			}
			/* else, input value is the correct output as well */

			EEO_NEXT();
		}

		EEO_CASE(EEOP_PARAM_EXEC)
		{
			/* out of line implementation: too large */
			ExecEvalParamExec(state, op, econtext,
							  RELPTR_RESOLVE(data, op->result));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_PARAM_EXTERN)
		{
			/* out of line implementation: too large */
			ExecEvalParamExtern(state, op, econtext,
								RELPTR_RESOLVE(data, op->result));
			EEO_NEXT();
		}

		EEO_CASE(EEOP_PARAM_CALLBACK)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);

			/* allow an extension module to supply a PARAM_EXTERN value */
			op->d.cparam.paramfunc(op->d.cparam.paramarg, econtext,
								   &op->d.cparam.param, opres);
			EEO_NEXT();
		}

		EEO_CASE(EEOP_CASE_TESTVAL)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);

			/*
			 * Normally upper parts of the expression tree have setup the
			 * values to be returned here, but some parts of the system
			 * currently misuse {caseValue,domainValue}_{datum,isNull} to set
			 * run-time data.  So if no values have been set-up, use
			 * ExprContext's.  This isn't pretty, but also not *that* ugly,
			 * and this is unlikely to be performance sensitive enough to
			 * worry about an extra branch.
			 */
			if (op->d.casetest.value.ptr.allocno != 0)
			{
				NullableDatum *caseval =
					RELPTR_RESOLVE(data, op->d.casetest.value);

				*opres = *caseval;
			}
			else
			{
				*opres = econtext->caseValue;
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_DOMAIN_TESTVAL)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);

			/*
			 * See EEOP_CASE_TESTVAL comment.
			 */
			if (op->d.casetest.value.ptr.allocno != 0)
			{
				NullableDatum *caseval =
					RELPTR_RESOLVE(data, op->d.casetest.value);

				*opres = *caseval;
			}
			else
			{
				*opres = econtext->domainValue;
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_MAKE_READONLY)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);
			NullableDatum *roval =
				RELPTR_RESOLVE(data, op->d.make_readonly.value);

			/*
			 * Force a varlena value that might be read multiple times to R/O
			 */
			if (!roval->isnull)
				opres->value =
					MakeExpandedObjectReadOnlyInternal(roval->value);
			opres->isnull = roval->isnull;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_IOCOERCE)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);

			/*
			 * Evaluate a CoerceViaIO node.  This can be quite a hot path, so
			 * inline as much work as possible.  The source value is in our
			 * result variable.
			 */
			char	   *str;

			/* call output function (similar to OutputFunctionCall) */
			if (opres->isnull)
			{
				/* output functions are not called on nulls */
				str = NULL;
			}
			else
			{
				FunctionCallInfo fcinfo_out =
					RELPTR_RESOLVE(data, op->d.iocoerce.fcinfo_data_out);
				PGFunction fn_addr_out = op->d.iocoerce.fn_addr_out;

				fcinfo_out->args[0] = *opres;

				fcinfo_out->isnull = false;
				str = DatumGetCString(fn_addr_out(fcinfo_out));

				/* OutputFunctionCall assumes result isn't null */
				Assert(!fcinfo_out->isnull);
			}

			/* call input function (similar to InputFunctionCall) */
			if (!op->d.iocoerce.fn_strict_in || str != NULL)
			{
				FunctionCallInfo fcinfo_in =
					RELPTR_RESOLVE(data, op->d.iocoerce.fcinfo_data_in);
				PGFunction fn_addr_in = op->d.iocoerce.fn_addr_in;
				Datum		d;

				fcinfo_in->args[0].value = PointerGetDatum(str);
				fcinfo_in->args[0].isnull = opres->isnull;
				/* second and third arguments are already set up */

				fcinfo_in->isnull = false;
				d = fn_addr_in(fcinfo_in);
				opres->value = d;

				/* Should get null result if and only if str is NULL */
				if (str == NULL)
				{
					Assert(opres->isnull);
					Assert(fcinfo_in->isnull);
				}
				else
				{
					Assert(!opres->isnull);
					Assert(!fcinfo_in->isnull);
				}
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_DISTINCT)
		{
			/*
			 * IS DISTINCT FROM must evaluate arguments (already done into
			 * fcinfo->args) to determine whether they are NULL; if either is
			 * NULL then the result is determined.  If neither is NULL, then
			 * proceed to evaluate the comparison function, which is just the
			 * type's standard equality operator.  We need not care whether
			 * that function is strict.  Because the handling of nulls is
			 * different, we can't just reuse EEOP_FUNCEXPR.
			 */
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);
			FunctionCallInfo fcinfo =
				RELPTR_RESOLVE(data, op->d.func.fcinfo_data);

			/* check function arguments for NULLness */
			if (fcinfo->args[0].isnull && fcinfo->args[1].isnull)
			{
				/* Both NULL? Then is not distinct... */
				opres->value = BoolGetDatum(false);
				opres->isnull = false;
			}
			else if (fcinfo->args[0].isnull || fcinfo->args[1].isnull)
			{
				/* Only one is NULL? Then is distinct... */
				opres->value = BoolGetDatum(true);
				opres->isnull = false;
			}
			else
			{
				/* Neither null, so apply the equality function */
				Datum		eqresult;

				fcinfo->isnull = false;
				eqresult = op->d.func.fn_addr(fcinfo);
				/* Must invert result of "="; safe to do even if null */
				opres->value = BoolGetDatum(!DatumGetBool(eqresult));
				opres->isnull = fcinfo->isnull;
			}

			EEO_NEXT();
		}

		/* see EEOP_DISTINCT for comments, this is just inverted */
		EEO_CASE(EEOP_NOT_DISTINCT)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);
			FunctionCallInfo fcinfo =
				RELPTR_RESOLVE(data, op->d.func.fcinfo_data);

			if (fcinfo->args[0].isnull && fcinfo->args[1].isnull)
			{
				opres->value = BoolGetDatum(true);
				opres->isnull = false;
			}
			else if (fcinfo->args[0].isnull || fcinfo->args[1].isnull)
			{
				opres->value = BoolGetDatum(false);
				opres->isnull = false;
			}
			else
			{
				Datum		eqresult;

				fcinfo->isnull = false;
				eqresult = op->d.func.fn_addr(fcinfo);
				opres->value = eqresult;
				opres->isnull = fcinfo->isnull;
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NULLIF)
		{
			/*
			 * The arguments are already evaluated into fcinfo->args.
			 */
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);
			FunctionCallInfo fcinfo =
				RELPTR_RESOLVE(data, op->d.func.fcinfo_data);

			/* if either argument is NULL they can't be equal */
			if (!fcinfo->args[0].isnull && !fcinfo->args[1].isnull)
			{
				Datum		result;

				fcinfo->isnull = false;
				result = op->d.func.fn_addr(fcinfo);

				/* if the arguments are equal return null */
				if (!fcinfo->isnull && DatumGetBool(result))
				{
					*opres = NULL_DATUM;

					EEO_NEXT();
				}
			}

			/* Arguments aren't equal, so return the first one */
			*opres = fcinfo->args[0];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SQLVALUEFUNCTION)
		{
			/*
			 * Doesn't seem worthwhile to have an inline implementation
			 * efficiency-wise.
			 */
			ExecEvalSQLValueFunction(state, op,
									 RELPTR_RESOLVE(data, op->result));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CURRENTOFEXPR)
		{
			/* error invocation uses space, and shouldn't ever occur */
			ExecEvalCurrentOfExpr(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NEXTVALUEEXPR)
		{
			/*
			 * Doesn't seem worthwhile to have an inline implementation
			 * efficiency-wise.
			 */
			ExecEvalNextValueExpr(state, op,
								  RELPTR_RESOLVE(data, op->result));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ARRAYEXPR)
		{
			/* too complex for an inline implementation */
			ExecEvalArrayExpr(state, op,
							  RELPTR_RESOLVE(data, op->result),
							  RELPTR_RESOLVE(data, op->d.arrayexpr.elements));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ARRAYCOERCE_RELABEL)
		{
			/* too complex for an inline implementation */
			ExecEvalArrayCoerceRelabel(state, op,
									   RELPTR_RESOLVE(data, op->result));
			EEO_NEXT();
		}

		EEO_CASE(EEOP_ARRAYCOERCE_UNPACK)
		{
			/* too complex for an inline implementation */
			if (ExecEvalArrayCoerceUnpack(state, op,
										  RELPTR_RESOLVE(data, op->result)))
				EEO_NEXT(); /* already done */
			EEO_JUMP(op->d.arraycoerce.jumpnext);
		}

		EEO_CASE(EEOP_ARRAYCOERCE_PACK)
		{
			/* too complex for an inline implementation */
			if (ExecEvalArrayCoercePack(state, op,
										RELPTR_RESOLVE(data, op->result)))
				EEO_JUMP(op->d.arraycoerce.jumpnext); /* transform next element */
			else
				EEO_NEXT(); /* finished */
		}

		EEO_CASE(EEOP_ROW)
		{
			/* too complex for an inline implementation */
			ExecEvalRow(state, op,
						RELPTR_RESOLVE(data, op->result),
						RELPTR_RESOLVE(data, op->d.row.elements));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ROWCOMPARE_STEP)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);
			FunctionCallInfo fcinfo =
				RELPTR_RESOLVE(data, op->d.rowcompare_step.fcinfo_data);
			Datum		d;

			/* force NULL result if strict fn and NULL input */
			if (op->d.rowcompare_step.fn_strict &&
				(fcinfo->args[0].isnull || fcinfo->args[1].isnull))
			{
				opres->isnull = true;
				EEO_JUMP(op->d.rowcompare_step.jumpnull);
			}

			/* Apply comparison function */
			fcinfo->isnull = false;
			d = op->d.rowcompare_step.fn_addr(fcinfo);
			opres->value = d;
			opres->isnull = fcinfo->isnull;

			/* force NULL result if NULL function result */
			if (fcinfo->isnull)
				EEO_JUMP(op->d.rowcompare_step.jumpnull);

			/* If unequal, no need to compare remaining columns */
			if (DatumGetInt32(opres->value) != 0)
				EEO_JUMP(op->d.rowcompare_step.jumpdone);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ROWCOMPARE_FINAL)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);
			RowCompareType rctype = op->d.rowcompare_final.rctype;
			int32		cmpresult = DatumGetInt32(opres->value);

			opres->isnull = false;
			switch (rctype)
			{
					/* EQ and NE cases aren't allowed here */
				case ROWCOMPARE_LT:
					opres->value = BoolGetDatum(cmpresult < 0);
					break;
				case ROWCOMPARE_LE:
					opres->value = BoolGetDatum(cmpresult <= 0);
					break;
				case ROWCOMPARE_GE:
					opres->value = BoolGetDatum(cmpresult >= 0);
					break;
				case ROWCOMPARE_GT:
					opres->value = BoolGetDatum(cmpresult > 0);
					break;
				default:
					Assert(false);
					break;
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_MINMAX)
		{
			/* too complex for an inline implementation */
			ExecEvalMinMax(state, op,
						   RELPTR_RESOLVE(data, op->result),
						   RELPTR_RESOLVE(data, op->d.minmax.arguments),
						   RELPTR_RESOLVE(data, op->d.minmax.fcinfo_data));
			EEO_NEXT();
		}

		EEO_CASE(EEOP_FIELDSELECT)
		{
			/* too complex for an inline implementation */
			ExecEvalFieldSelect(state, nonconst_op, econtext,
								RELPTR_RESOLVE(data, op->result));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_FIELDSTORE_DEFORM)
		{
			/* too complex for an inline implementation */
			ExecEvalFieldStoreDeForm(state, op, econtext,
									 RELPTR_RESOLVE(data, op->result),
									 RELPTR_RESOLVE(data, op->d.fieldstore.columns));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_FIELDSTORE_FORM)
		{
			/* too complex for an inline implementation */
			ExecEvalFieldStoreForm(state, op,
								   RELPTR_RESOLVE(data, op->result),
								   RELPTR_RESOLVE(data, op->d.fieldstore.columns));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SBSREF_SUBSCRIPTS)
		{
			bool ret;

			/* Precheck SubscriptingRef subscript(s) */
			ret = op->d.sbsref_subscript.subscriptfunc(
				econtext, op->d.sbsref_subscript.state,
				RELPTR_RESOLVE(data, op->d.sbsref_subscript.state->lowerindex),
				RELPTR_RESOLVE(data, op->d.sbsref_subscript.state->upperindex),
				RELPTR_RESOLVE(data, op->result));

			if (ret)
			{
				EEO_NEXT();
			}
			else
			{
				/* Subscript is null, short-circuit SubscriptingRef to NULL */
				EEO_JUMP(op->d.sbsref_subscript.jumpdone);
			}
		}

		EEO_CASE(EEOP_SBSREF_OLD)
			EEO_CASE(EEOP_SBSREF_ASSIGN)
			EEO_CASE(EEOP_SBSREF_FETCH)
		{
			/* Perform a SubscriptingRef fetch or assignment */
			op->d.sbsref.subscriptfunc(econtext, op->d.sbsref.state,
									   RELPTR_RESOLVE(data, op->d.sbsref.state->lowerindex),
									   RELPTR_RESOLVE(data, op->d.sbsref.state->upperindex),
									   RELPTR_RESOLVE(data, op->d.sbsref.state->replace),
									   RELPTR_RESOLVE(data, op->d.sbsref.state->prev),
									   RELPTR_RESOLVE(data, op->result));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CONVERT_ROWTYPE)
		{
			/* too complex for an inline implementation */
			ExecEvalConvertRowtype(state, nonconst_op, econtext,
								   RELPTR_RESOLVE(data, op->result));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SCALARARRAYOP)
		{
			/* too complex for an inline implementation */
			ExecEvalScalarArrayOp(state, nonconst_op,
								  RELPTR_RESOLVE(data, op->result),
								  RELPTR_RESOLVE(data, op->d.scalararrayop.fcinfo_data));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_HASHED_SCALARARRAYOP)
		{
			/* too complex for an inline implementation */
			ExecEvalHashedScalarArrayOp(state, nonconst_op, econtext,
										RELPTR_RESOLVE(data, op->result),
										RELPTR_RESOLVE(data, op->d.hashedscalararrayop.fcinfo_data));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_DOMAIN_NOTNULL)
		{
			/* too complex for an inline implementation */
			ExecEvalConstraintNotNull(state, op,
									  RELPTR_RESOLVE(data, op->result));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_DOMAIN_CHECK)
		{
			/* too complex for an inline implementation */
			ExecEvalConstraintCheck(state, op,
									RELPTR_RESOLVE(data, op->d.domaincheck.check));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_XMLEXPR)
		{
			/* too complex for an inline implementation */
			ExecEvalXmlExpr(state, op,
							RELPTR_RESOLVE(data, op->result),
							RELPTR_RESOLVE_MAYNULL(data, op->d.xmlexpr.args),
							RELPTR_RESOLVE_MAYNULL(data, op->d.xmlexpr.named_args));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_JSON_CONSTRUCTOR)
		{
			/* too complex for an inline implementation */
			ExecEvalJsonConstructor(state, op, econtext,
									RELPTR_RESOLVE(data, op->result),
									RELPTR_RESOLVE(data, op->d.json_constructor.jcstate->arg_values));
			EEO_NEXT();
		}

		EEO_CASE(EEOP_IS_JSON)
		{
			/* too complex for an inline implementation */
			ExecEvalJsonIsPredicate(state, op,
									RELPTR_RESOLVE(data, op->result));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_AGGREF)
		{
			/*
			 * Returns a Datum whose value is the precomputed aggregate value
			 * found in the given expression context.
			 */
			int			aggno = op->d.aggref.aggno;
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);

			*opres = econtext->ecxt_aggvalues[aggno];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_GROUPING_FUNC)
		{
			/* too complex/uncommon for an inline implementation */
			ExecEvalGroupingFunc(state, op,
								 RELPTR_RESOLVE(data, op->result));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_WINDOW_FUNC)
		{
			/*
			 * Like Aggref, just return a precomputed value from the econtext.
			 */
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);
			WindowFuncExprState *wfunc = op->d.window_func.wfstate;

			Assert(econtext->ecxt_aggvalues != NULL);

			*opres = econtext->ecxt_aggvalues[wfunc->wfuncno];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SUBPLAN)
		{
			/* too complex for an inline implementation */
			ExecEvalSubPlan(state, op, econtext,
							RELPTR_RESOLVE(data, op->result));

			EEO_NEXT();
		}

		/* evaluate a strict aggregate deserialization function */
		EEO_CASE(EEOP_AGG_STRICT_DESERIALIZE)
		{
			FunctionCallInfo fcinfo =
				RELPTR_RESOLVE(data, op->d.agg_deserialize.fcinfo_data);
			/* Fallthrough state, no need to resolve FmgrInfo here */

			/* Don't call a strict deserialization function with NULL input */
			if (fcinfo->args[0].isnull)
				EEO_JUMP(op->d.agg_deserialize.jumpnull);

			/* fallthrough */
		}

		/* evaluate aggregate deserialization function (non-strict portion) */
		EEO_CASE(EEOP_AGG_DESERIALIZE)
		{
			NullableDatum *opres =
				RELPTR_RESOLVE(data, op->result);
			FunctionCallInfo fcinfo =
				RELPTR_RESOLVE(data, op->d.agg_deserialize.fcinfo_data);
			//FmgrInfo *finfo = RELPTR_RESOLVE(data, op->d.agg_deserialize.finfo);
			AggState   *aggstate = castNode(AggState, state->parent);
			MemoryContext oldContext;

			/*
			 * We run the deserialization functions in per-input-tuple memory
			 * context.
			 */
			oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);
			fcinfo->isnull = false;
			opres->value = op->d.agg_deserialize.fn_addr(fcinfo);
			//opres->value = finfo->fn_addr(fcinfo);
			opres->isnull = fcinfo->isnull;
			MemoryContextSwitchTo(oldContext);

			EEO_NEXT();
		}

		/*
		 * Check that a strict aggregate transition / combination function's
		 * input is not NULL.
		 */

		/* when checking more than one argument */
		EEO_CASE(EEOP_AGG_STRICT_INPUT_CHECK_ARGS)
		{
			NullableDatum *args =
				RELPTR_RESOLVE(data, op->d.agg_strict_input_check.args);
			int			nargs = op->d.agg_strict_input_check.nargs;

			Assert(nargs > 1);

			for (int argno = 0; argno < nargs; argno++)
			{
				if (args[argno].isnull)
					EEO_JUMP(op->d.agg_strict_input_check.jumpnull);
			}
			EEO_NEXT();
		}

		/* special case for just one argument */
		EEO_CASE(EEOP_AGG_STRICT_INPUT_CHECK_ARGS_1)
		{
			NullableDatum *args =
				RELPTR_RESOLVE(data, op->d.agg_strict_input_check.args);

			Assert(op->d.agg_strict_input_check.nargs == 1);

			if (args[0].isnull)
				EEO_JUMP(op->d.agg_strict_input_check.jumpnull);
			EEO_NEXT();
		}

		/*
		 * Check for a NULL pointer to the per-group states.
		 */

		EEO_CASE(EEOP_AGG_PLAIN_PERGROUP_NULLCHECK)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			AggStatePerGroup pergroup_allaggs =
				aggstate->all_pergroups[op->d.agg_plain_pergroup_nullcheck.setoff];

			if (pergroup_allaggs == NULL)
				EEO_JUMP(op->d.agg_plain_pergroup_nullcheck.jumpnull);

			EEO_NEXT();
		}

		/*
		 * Different types of aggregate transition functions are implemented
		 * as different types of steps, to avoid incurring unnecessary
		 * overhead.  There's a step type for each valid combination of having
		 * a by value / by reference transition type, [not] needing to the
		 * initialize the transition value for the first row in a group from
		 * input, and [not] strict transition function.
		 *
		 * Could optimize further by splitting off by-reference for
		 * fixed-length types, but currently that doesn't seem worth it.
		 */

		EEO_CASE(EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			const AggStatePerCallContext *percall = op->d.agg_trans.percall;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];
			FunctionCallInfo fcinfo =
				RELPTR_RESOLVE(data, op->d.agg_trans.trans_fcinfo);

			Assert(percall->pertrans->transtypeByVal);

			if (pergroup->noTransValue)
			{
				/* If transValue has not yet been initialized, do so now. */
				ExecAggInitGroup(percall, pergroup, fcinfo);

				/* copied trans value from input, done this round */
			}
			else if (likely(!pergroup->transValue.isnull))
			{
				/* invoke transition function, unless prevented by strictness */
				ExecAggPlainTransByVal(aggstate, percall, pergroup,
									   fcinfo, op->d.agg_trans.fn_addr);
			}

			EEO_NEXT();
		}

		/* see comments above EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL */
		EEO_CASE(EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			const AggStatePerCallContext *percall = op->d.agg_trans.percall;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];
			FunctionCallInfo fcinfo =
				RELPTR_RESOLVE(data, op->d.agg_trans.trans_fcinfo);

			Assert(percall->pertrans->transtypeByVal);

			if (likely(!pergroup->transValue.isnull))
				ExecAggPlainTransByVal(aggstate, percall, pergroup,
									   fcinfo,
									   op->d.agg_trans.fn_addr);

			EEO_NEXT();
		}

		/* see comments above EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL */
		EEO_CASE(EEOP_AGG_PLAIN_TRANS_BYVAL)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			const AggStatePerCallContext *percall = op->d.agg_trans.percall;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];
			FunctionCallInfo fcinfo =
				RELPTR_RESOLVE(data, op->d.agg_trans.trans_fcinfo);

			Assert(percall->pertrans->transtypeByVal);

			ExecAggPlainTransByVal(aggstate, percall, pergroup,
								   fcinfo,
								   op->d.agg_trans.fn_addr);

			EEO_NEXT();
		}

		/* see comments above EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL */
		EEO_CASE(EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			const AggStatePerCallContext *percall = op->d.agg_trans.percall;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];
			FunctionCallInfo fcinfo =
				RELPTR_RESOLVE(data, op->d.agg_trans.trans_fcinfo);

			Assert(!percall->pertrans->transtypeByVal);

			if (pergroup->noTransValue)
				ExecAggInitGroup(percall, pergroup, fcinfo);
			else if (likely(!pergroup->transValue.isnull))
				ExecAggPlainTransByRef(aggstate, percall, pergroup,
									   fcinfo,
									   op->d.agg_trans.fn_addr);

			EEO_NEXT();
		}

		/* see comments above EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL */
		EEO_CASE(EEOP_AGG_PLAIN_TRANS_STRICT_BYREF)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			const AggStatePerCallContext *percall = op->d.agg_trans.percall;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];
			FunctionCallInfo fcinfo =
				RELPTR_RESOLVE(data, op->d.agg_trans.trans_fcinfo);

			Assert(!percall->pertrans->transtypeByVal);

			if (likely(!pergroup->transValue.isnull))
				ExecAggPlainTransByRef(aggstate, percall, pergroup,
									   fcinfo,
									   op->d.agg_trans.fn_addr);
			EEO_NEXT();
		}

		/* see comments above EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL */
		EEO_CASE(EEOP_AGG_PLAIN_TRANS_BYREF)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			const AggStatePerCallContext *percall = op->d.agg_trans.percall;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];
			FunctionCallInfo fcinfo =
				RELPTR_RESOLVE(data, op->d.agg_trans.trans_fcinfo);

			Assert(!percall->pertrans->transtypeByVal);

			ExecAggPlainTransByRef(aggstate, percall, pergroup,
								   fcinfo,
								   op->d.agg_trans.fn_addr);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_AGG_PRESORTED_DISTINCT_SINGLE)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			const AggStatePerCallContext *percall = op->d.agg_presorted_distinctcheck.percall;
			AggStatePerTrans pertrans = percall->pertrans;
			FunctionCallInfo fcinfo =
				RELPTR_RESOLVE(data, op->d.agg_presorted_distinctcheck.trans_fcinfo);

			if (ExecEvalPreOrderedDistinctSingle(percall, aggstate, pertrans, fcinfo))
				EEO_NEXT();
			else
				EEO_JUMP(op->d.agg_presorted_distinctcheck.jumpdistinct);
		}

		EEO_CASE(EEOP_AGG_PRESORTED_DISTINCT_MULTI)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			const AggStatePerCallContext *percall = op->d.agg_presorted_distinctcheck.percall;
			AggStatePerTrans pertrans = percall->pertrans;
			FunctionCallInfo fcinfo =
				RELPTR_RESOLVE(data, op->d.agg_presorted_distinctcheck.trans_fcinfo);

			if (ExecEvalPreOrderedDistinctMulti(percall, aggstate, pertrans, fcinfo))
				EEO_NEXT();
			else
				EEO_JUMP(op->d.agg_presorted_distinctcheck.jumpdistinct);
		}

		/* process single-column ordered aggregate datum */
		EEO_CASE(EEOP_AGG_ORDERED_TRANS_DATUM)
		{
			/* too complex for an inline implementation */
			ExecEvalAggOrderedTransDatum(state, op,
										 RELPTR_RESOLVE(data, op->d.agg_trans_ordered.columns));

			EEO_NEXT();
		}

		/* process multi-column ordered aggregate tuple */
		EEO_CASE(EEOP_AGG_ORDERED_TRANS_TUPLE)
		{
			/* too complex for an inline implementation */
			ExecEvalAggOrderedTransTuple(state, op,
										 RELPTR_RESOLVE(data, op->d.agg_trans_ordered.columns));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_LAST)
		{
			/* unreachable */
			Assert(false);
			goto out_error;
		}
	}

out_error:
	pg_unreachable();
	return (Datum) 0;
}

/*
 * Expression evaluation callback that performs extra checks before executing
 * the expression. Declared extern so other methods of execution can use it
 * too.
 */
Datum
ExecInterpExprStillValid(ExprState *state, ExprContext *econtext, bool *isNull)
{
	/*
	 * First time through, check whether attribute matches Var.  Might not be
	 * ok anymore, due to schema changes.
	 */
	CheckExprStillValid(state, econtext);

	/* skip the check during further executions */
	state->evalfunc = (ExprStateEvalFunc) state->evalfunc_private;

	/* and actually execute */
	return state->evalfunc(state, econtext, isNull);
}

/*
 * Check that an expression is still valid in the face of potential schema
 * changes since the plan has been created.
 */
void
CheckExprStillValid(ExprState *state, ExprContext *econtext)
{
	TupleTableSlot *innerslot;
	TupleTableSlot *outerslot;
	TupleTableSlot *scanslot;

	innerslot = econtext->ecxt_innertuple;
	outerslot = econtext->ecxt_outertuple;
	scanslot = econtext->ecxt_scantuple;

	for (int i = 0; i < state->steps_final_len; i++)
	{
		const ExprEvalStep *op = &state->steps[i];

		switch (ExecEvalStepOp(state, op))
		{
			case EEOP_INNER_VAR:
				{
					int			attnum = op->d.var.attnum;

					CheckVarSlotCompatibility(innerslot, attnum + 1, op->d.var.vartype);
					break;
				}

			case EEOP_OUTER_VAR:
				{
					int			attnum = op->d.var.attnum;

					CheckVarSlotCompatibility(outerslot, attnum + 1, op->d.var.vartype);
					break;
				}

			case EEOP_SCAN_VAR:
				{
					int			attnum = op->d.var.attnum;

					CheckVarSlotCompatibility(scanslot, attnum + 1, op->d.var.vartype);
					break;
				}
			default:
				break;
		}
	}
}

/*
 * Check whether a user attribute in a slot can be referenced by a Var
 * expression.  This should succeed unless there have been schema changes
 * since the expression tree has been created.
 */
static void
CheckVarSlotCompatibility(TupleTableSlot *slot, int attnum, Oid vartype)
{
	/*
	 * What we have to check for here is the possibility of an attribute
	 * having been dropped or changed in type since the plan tree was created.
	 * Ideally the plan will get invalidated and not re-used, but just in
	 * case, we keep these defenses.  Fortunately it's sufficient to check
	 * once on the first time through.
	 *
	 * Note: ideally we'd check typmod as well as typid, but that seems
	 * impractical at the moment: in many cases the tupdesc will have been
	 * generated by ExecTypeFromTL(), and that can't guarantee to generate an
	 * accurate typmod in all cases, because some expression node types don't
	 * carry typmod.  Fortunately, for precisely that reason, there should be
	 * no places with a critical dependency on the typmod of a value.
	 *
	 * System attributes don't require checking since their types never
	 * change.
	 */
	if (attnum > 0)
	{
		TupleDesc	slot_tupdesc = slot->tts_tupleDescriptor;
		Form_pg_attribute attr;

		if (attnum > slot_tupdesc->natts)	/* should never happen */
			elog(ERROR, "attribute number %d exceeds number of columns %d",
				 attnum, slot_tupdesc->natts);

		attr = TupleDescAttr(slot_tupdesc, attnum - 1);

		if (attr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("attribute %d of type %s has been dropped",
							attnum, format_type_be(slot_tupdesc->tdtypeid))));

		if (vartype != attr->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("attribute %d of type %s has wrong type",
							attnum, format_type_be(slot_tupdesc->tdtypeid)),
					 errdetail("Table has type %s, but query expects %s.",
							   format_type_be(attr->atttypid),
							   format_type_be(vartype))));
	}
}

/*
 * Verify that the slot is compatible with a EEOP_*_FETCHSOME operation.
 */
static void
CheckOpSlotCompatibility(const ExprEvalStep *op, TupleTableSlot *slot)
{
#ifdef USE_ASSERT_CHECKING
	/* there's nothing to check */
	if (!op->d.fetch.fixed)
		return;

	/*
	 * Should probably fixed at some point, but for now it's easier to allow
	 * buffer and heap tuples to be used interchangeably.
	 */
	if (slot->tts_ops == &TTSOpsBufferHeapTuple &&
		op->d.fetch.kind == &TTSOpsHeapTuple)
		return;
	if (slot->tts_ops == &TTSOpsHeapTuple &&
		op->d.fetch.kind == &TTSOpsBufferHeapTuple)
		return;

	/*
	 * At the moment we consider it OK if a virtual slot is used instead of a
	 * specific type of slot, as a virtual slot never needs to be deformed.
	 */
	if (slot->tts_ops == &TTSOpsVirtual)
		return;

	Assert(op->d.fetch.kind == slot->tts_ops);
#endif
}

/*
 * get_cached_rowtype: utility function to lookup a rowtype tupdesc
 *
 * type_id, typmod: identity of the rowtype
 * rowcache: space for caching identity info
 *		(rowcache->cacheptr must be initialized to NULL)
 * changed: if not NULL, *changed is set to true on any update
 *
 * The returned TupleDesc is not guaranteed pinned; caller must pin it
 * to use it across any operation that might incur cache invalidation,
 * including for example detoasting of input tuples.
 * (The TupleDesc is always refcounted, so just use IncrTupleDescRefCount.)
 *
 * NOTE: because composite types can change contents, we must be prepared
 * to re-do this during any node execution; cannot call just once during
 * expression initialization.
 */
static TupleDesc
get_cached_rowtype(Oid type_id, int32 typmod,
				   ExprEvalRowtypeCache *rowcache,
				   bool *changed)
{
	if (type_id != RECORDOID)
	{
		/*
		 * It's a named composite type, so use the regular typcache.  Do a
		 * lookup first time through, or if the composite type changed.  Note:
		 * "tupdesc_id == 0" may look redundant, but it protects against the
		 * admittedly-theoretical possibility that type_id was RECORDOID the
		 * last time through, so that the cacheptr isn't TypeCacheEntry *.
		 */
		TypeCacheEntry *typentry = (TypeCacheEntry *) rowcache->cacheptr;

		if (unlikely(typentry == NULL ||
					 rowcache->tupdesc_id == 0 ||
					 typentry->tupDesc_identifier != rowcache->tupdesc_id))
		{
			typentry = lookup_type_cache(type_id, TYPECACHE_TUPDESC);
			if (typentry->tupDesc == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("type %s is not composite",
								format_type_be(type_id))));
			rowcache->cacheptr = (void *) typentry;
			rowcache->tupdesc_id = typentry->tupDesc_identifier;
			if (changed)
				*changed = true;
		}
		return typentry->tupDesc;
	}
	else
	{
		/*
		 * A RECORD type, once registered, doesn't change for the life of the
		 * backend.  So we don't need a typcache entry as such, which is good
		 * because there isn't one.  It's possible that the caller is asking
		 * about a different type than before, though.
		 */
		TupleDesc	tupDesc = (TupleDesc) rowcache->cacheptr;

		if (unlikely(tupDesc == NULL ||
					 rowcache->tupdesc_id != 0 ||
					 type_id != tupDesc->tdtypeid ||
					 typmod != tupDesc->tdtypmod))
		{
			tupDesc = lookup_rowtype_tupdesc(type_id, typmod);
			/* Drop pin acquired by lookup_rowtype_tupdesc */
			ReleaseTupleDesc(tupDesc);
			rowcache->cacheptr = (void *) tupDesc;
			rowcache->tupdesc_id = 0;	/* not a valid value for non-RECORD */
			if (changed)
				*changed = true;
		}
		return tupDesc;
	}
}


/*
 * Fast-path functions, for very simple expressions
 */

/* implementation of ExecJust(Inner|Outer|Scan)Var */
static pg_attribute_always_inline Datum
ExecJustVarImpl(ExprState *state, TupleTableSlot *slot, bool *isnull)
{
	const ExprEvalStep *op = &state->steps[1];
	int			attnum = op->d.var.attnum + 1;

	CheckOpSlotCompatibility(&state->steps[0], slot);

	/*
	 * Since we use slot_getattr(), we don't need to implement the FETCHSOME
	 * step explicitly, and we also needn't Assert that the attnum is in range
	 * --- slot_getattr() will take care of any problems.
	 */
	return slot_getattr(slot, attnum, isnull);
}

/* Simple reference to inner Var */
static Datum
ExecJustInnerVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustVarImpl(state, econtext->ecxt_innertuple, isnull);
}

/* Simple reference to outer Var */
static Datum
ExecJustOuterVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustVarImpl(state, econtext->ecxt_outertuple, isnull);
}

/* Simple reference to scan Var */
static Datum
ExecJustScanVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustVarImpl(state, econtext->ecxt_scantuple, isnull);
}

/* implementation of ExecJustAssign(Inner|Outer|Scan)Var */
static pg_attribute_always_inline Datum
ExecJustAssignVarImpl(ExprState *state, TupleTableSlot *inslot, bool *isnull)
{
	const ExprEvalStep *op = &state->steps[1];
	int			attnum = op->d.assign_var.attnum + 1;
	int			resultnum = op->d.assign_var.resultnum;
	TupleTableSlot *outslot = state->resultslot;
	NullableDatum *outval = &outslot->tts_values[resultnum];

	CheckOpSlotCompatibility(&state->steps[0], inslot);

	/*
	 * We do not need CheckVarSlotCompatibility here; that was taken care of
	 * at compilation time.
	 *
	 * Since we use slot_getattr(), we don't need to implement the FETCHSOME
	 * step explicitly, and we also needn't Assert that the attnum is in range
	 * --- slot_getattr() will take care of any problems.  Nonetheless, check
	 * that resultnum is in range.
	 */
	Assert(resultnum >= 0 && resultnum < outslot->tts_tupleDescriptor->natts);
	outval->value = slot_getattr(inslot, attnum, &outval->isnull);
	return 0;
}

/* Evaluate inner Var and assign to appropriate column of result tuple */
static Datum
ExecJustAssignInnerVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustAssignVarImpl(state, econtext->ecxt_innertuple, isnull);
}

/* Evaluate outer Var and assign to appropriate column of result tuple */
static Datum
ExecJustAssignOuterVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustAssignVarImpl(state, econtext->ecxt_outertuple, isnull);
}

/* Evaluate scan Var and assign to appropriate column of result tuple */
static Datum
ExecJustAssignScanVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustAssignVarImpl(state, econtext->ecxt_scantuple, isnull);
}

/* Simple Const expression */
static Datum
ExecJustConst(ExprState *state, ExprContext *econtext, bool *isnull)
{
	const ExprEvalStep *op = &state->steps[0];

	*isnull = op->d.constval.value.isnull;
	return op->d.constval.value.value;
}

/* implementation of ExecJust(Inner|Outer|Scan)VarVirt */
static pg_attribute_always_inline Datum
ExecJustVarVirtImpl(ExprState *state, TupleTableSlot *slot, bool *isnull)
{
	const ExprEvalStep *op = &state->steps[0];
	int			attnum = op->d.var.attnum;

	/*
	 * As it is guaranteed that a virtual slot is used, there never is a need
	 * to perform tuple deforming (nor would it be possible). Therefore
	 * execExpr.c has not emitted an EEOP_*_FETCHSOME step. Verify, as much as
	 * possible, that that determination was accurate.
	 */
	Assert(TTS_IS_VIRTUAL(slot));
	Assert(TTS_FIXED(slot));
	Assert(attnum >= 0 && attnum < slot->tts_nvalid);

	*isnull = slot->tts_values[attnum].isnull;
	return slot->tts_values[attnum].value;
}

/* Like ExecJustInnerVar, optimized for virtual slots */
static Datum
ExecJustInnerVarVirt(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustVarVirtImpl(state, econtext->ecxt_innertuple, isnull);
}

/* Like ExecJustOuterVar, optimized for virtual slots */
static Datum
ExecJustOuterVarVirt(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustVarVirtImpl(state, econtext->ecxt_outertuple, isnull);
}

/* Like ExecJustScanVar, optimized for virtual slots */
static Datum
ExecJustScanVarVirt(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustVarVirtImpl(state, econtext->ecxt_scantuple, isnull);
}

/* implementation of ExecJustAssign(Inner|Outer|Scan)VarVirt */
static pg_attribute_always_inline Datum
ExecJustAssignVarVirtImpl(ExprState *state, TupleTableSlot *inslot, bool *isnull)
{
	const ExprEvalStep *op = &state->steps[0];
	int			attnum = op->d.assign_var.attnum;
	int			resultnum = op->d.assign_var.resultnum;
	TupleTableSlot *outslot = state->resultslot;

	/* see ExecJustVarVirtImpl for comments */

	Assert(TTS_IS_VIRTUAL(inslot));
	Assert(TTS_FIXED(inslot));
	Assert(attnum >= 0 && attnum < inslot->tts_nvalid);
	Assert(resultnum >= 0 && resultnum < outslot->tts_tupleDescriptor->natts);

	outslot->tts_values[resultnum] = inslot->tts_values[attnum];

	return 0;
}

/* Like ExecJustAssignInnerVar, optimized for virtual slots */
static Datum
ExecJustAssignInnerVarVirt(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustAssignVarVirtImpl(state, econtext->ecxt_innertuple, isnull);
}

/* Like ExecJustAssignOuterVar, optimized for virtual slots */
static Datum
ExecJustAssignOuterVarVirt(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustAssignVarVirtImpl(state, econtext->ecxt_outertuple, isnull);
}

/* Like ExecJustAssignScanVar, optimized for virtual slots */
static Datum
ExecJustAssignScanVarVirt(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustAssignVarVirtImpl(state, econtext->ecxt_scantuple, isnull);
}

#if defined(EEO_USE_COMPUTED_GOTO)
/*
 * Comparator used when building address->opcode lookup table for
 * ExecEvalStepOp() in the threaded dispatch case.
 */
static int
dispatch_compare_ptr(const void *a, const void *b)
{
	const ExprEvalOpLookup *la = (const ExprEvalOpLookup *) a;
	const ExprEvalOpLookup *lb = (const ExprEvalOpLookup *) b;

	if (la->opcode < lb->opcode)
		return -1;
	else if (la->opcode > lb->opcode)
		return 1;
	return 0;
}
#endif

/*
 * Do one-time initialization of interpretation machinery.
 */
static void
ExecInitInterpreter(void)
{
#if defined(EEO_USE_COMPUTED_GOTO)
	/* Set up externally-visible pointer to dispatch table */
	if (dispatch_table == NULL)
	{
		dispatch_table = (const void **)
			DatumGetPointer(ExecInterpExpr(NULL, NULL, NULL));

		/* build reverse lookup table */
		for (int i = 0; i < EEOP_LAST; i++)
		{
			reverse_dispatch_table[i].opcode = dispatch_table[i];
			reverse_dispatch_table[i].op = (ExprEvalOp) i;
		}

		/* make it bsearch()able */
		qsort(reverse_dispatch_table,
			  EEOP_LAST /* nmembers */ ,
			  sizeof(ExprEvalOpLookup),
			  dispatch_compare_ptr);
	}
#endif
}

/*
 * Function to return the opcode of an expression step.
 *
 * When direct-threading is in use, ExprState->opcode isn't easily
 * decipherable. This function returns the appropriate enum member.
 */
ExprEvalOp
ExecEvalStepOp(ExprState *state, const ExprEvalStep *op)
{
#if defined(EEO_USE_COMPUTED_GOTO)
	if (state->flags & EEO_FLAG_DIRECT_THREADED)
	{
		ExprEvalOpLookup key;
		ExprEvalOpLookup *res;

		key.opcode = (void *) op->opcode;
		res = bsearch(&key,
					  reverse_dispatch_table,
					  EEOP_LAST /* nmembers */ ,
					  sizeof(ExprEvalOpLookup),
					  dispatch_compare_ptr);
		Assert(res);			/* unknown ops shouldn't get looked up */
		return res->op;
	}
#endif
	return (ExprEvalOp) op->opcode;
}


/*
 * Out-of-line helper functions for complex instructions.
 */

/*
 * Evaluate EEOP_FUNCEXPR_FUSAGE
 */
void
ExecEvalFuncExprFusage(ExprState *state, const ExprEvalStep *op,
					   NullableDatum *opres, FunctionCallInfo fcinfo)
{
	PgStat_FunctionCallUsage fcusage;
	Datum		d;

	pgstat_init_function_usage(fcinfo, &fcusage);

	fcinfo->isnull = false;
	d = op->d.func.fn_addr(fcinfo);
	opres->value = d;
	opres->isnull = fcinfo->isnull;

	pgstat_end_function_usage(&fcusage, true);
}

/*
 * Evaluate EEOP_FUNCEXPR_STRICT_FUSAGE
 */
void
ExecEvalFuncExprStrictFusage(ExprState *state, const ExprEvalStep *op,
							 NullableDatum *opres, FunctionCallInfo fcinfo)
{
	PgStat_FunctionCallUsage fcusage;
	NullableDatum *args = fcinfo->args;
	int			nargs = op->d.func.nargs;
	Datum		d;

	/* strict function, so check for NULL args */
	for (int argno = 0; argno < nargs; argno++)
	{
		if (args[argno].isnull)
		{
			opres->isnull = true;
			return;
		}
	}

	pgstat_init_function_usage(fcinfo, &fcusage);

	fcinfo->isnull = false;
	d = op->d.func.fn_addr(fcinfo);
	opres->value = d;
	opres->isnull = fcinfo->isnull;

	pgstat_end_function_usage(&fcusage, true);
}

/*
 * Evaluate a PARAM_EXEC parameter.
 *
 * PARAM_EXEC params (internal executor parameters) are stored in the
 * ecxt_param_exec_vals array, and can be accessed by array index.
 */
void
ExecEvalParamExec(ExprState *state, const ExprEvalStep *op, ExprContext *econtext,
				  NullableDatum *opres)
{
	ParamExecData *prm;

	prm = &(econtext->ecxt_param_exec_vals[op->d.param.paramid]);
	if (unlikely(prm->execPlan != NULL))
	{
		/* Parameter not evaluated yet, so go do it */
		ExecSetParamPlan(prm->execPlan, econtext);
		/* ExecSetParamPlan should have processed this param... */
		Assert(prm->execPlan == NULL);
	}
	opres->value = prm->value;
	opres->isnull = prm->isnull;
}

/*
 * Evaluate a PARAM_EXTERN parameter.
 *
 * PARAM_EXTERN parameters must be sought in ecxt_param_list_info.
 */
void
ExecEvalParamExtern(ExprState *state, const ExprEvalStep *op, ExprContext *econtext,
					NullableDatum *opres)
{
	ParamListInfo paramInfo = econtext->ecxt_param_list_info;
	int			paramId = op->d.param.paramid;

	if (likely(paramInfo &&
			   paramId > 0 && paramId <= paramInfo->numParams))
	{
		ParamExternData *prm;
		ParamExternData prmdata;

		/* give hook a chance in case parameter is dynamic */
		if (paramInfo->paramFetch != NULL)
			prm = paramInfo->paramFetch(paramInfo, paramId, false, &prmdata);
		else
			prm = &paramInfo->params[paramId - 1];

		if (likely(OidIsValid(prm->ptype)))
		{
			/* safety check in case hook did something unexpected */
			if (unlikely(prm->ptype != op->d.param.paramtype))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("type of parameter %d (%s) does not match that when preparing the plan (%s)",
								paramId,
								format_type_be(prm->ptype),
								format_type_be(op->d.param.paramtype))));
			opres->value = prm->value;
			opres->isnull = prm->isnull;
			return;
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("no value found for parameter %d", paramId)));
}

/*
 * Evaluate a SQLValueFunction expression.
 */
void
ExecEvalSQLValueFunction(ExprState *state, const ExprEvalStep *op,
						 NullableDatum *opres)
{
	LOCAL_FCINFO(fcinfo, 0);
	SQLValueFunction *svf = op->d.sqlvaluefunction.svf;

	opres->isnull = false;

	/*
	 * Note: current_schema() can return NULL.  current_user() etc currently
	 * cannot, but might as well code those cases the same way for safety.
	 */
	switch (svf->op)
	{
		case SVFOP_CURRENT_DATE:
			opres->value = DateADTGetDatum(GetSQLCurrentDate());
			break;
		case SVFOP_CURRENT_TIME:
		case SVFOP_CURRENT_TIME_N:
			opres->value = TimeTzADTPGetDatum(GetSQLCurrentTime(svf->typmod));
			break;
		case SVFOP_CURRENT_TIMESTAMP:
		case SVFOP_CURRENT_TIMESTAMP_N:
			opres->value = TimestampTzGetDatum(GetSQLCurrentTimestamp(svf->typmod));
			break;
		case SVFOP_LOCALTIME:
		case SVFOP_LOCALTIME_N:
			opres->value = TimeADTGetDatum(GetSQLLocalTime(svf->typmod));
			break;
		case SVFOP_LOCALTIMESTAMP:
		case SVFOP_LOCALTIMESTAMP_N:
			opres->value = TimestampGetDatum(GetSQLLocalTimestamp(svf->typmod));
			break;
		case SVFOP_CURRENT_ROLE:
		case SVFOP_CURRENT_USER:
		case SVFOP_USER:
			InitFunctionCallInfoData(*fcinfo, NULL, 0, InvalidOid, NULL, NULL);
			opres->value = current_user(fcinfo);
			opres->isnull = fcinfo->isnull;
			break;
		case SVFOP_SESSION_USER:
			InitFunctionCallInfoData(*fcinfo, NULL, 0, InvalidOid, NULL, NULL);
			opres->value = session_user(fcinfo);
			opres->isnull = fcinfo->isnull;
			break;
		case SVFOP_CURRENT_CATALOG:
			InitFunctionCallInfoData(*fcinfo, NULL, 0, InvalidOid, NULL, NULL);
			opres->value = current_database(fcinfo);
			opres->isnull = fcinfo->isnull;
			break;
		case SVFOP_CURRENT_SCHEMA:
			InitFunctionCallInfoData(*fcinfo, NULL, 0, InvalidOid, NULL, NULL);
			opres->value = current_schema(fcinfo);
			opres->isnull = fcinfo->isnull;
			break;
	}
}

/*
 * Raise error if a CURRENT OF expression is evaluated.
 *
 * The planner should convert CURRENT OF into a TidScan qualification, or some
 * other special handling in a ForeignScan node.  So we have to be able to do
 * ExecInitExpr on a CurrentOfExpr, but we shouldn't ever actually execute it.
 * If we get here, we suppose we must be dealing with CURRENT OF on a foreign
 * table whose FDW doesn't handle it, and complain accordingly.
 */
void
ExecEvalCurrentOfExpr(ExprState *state, const ExprEvalStep *op)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("WHERE CURRENT OF is not supported for this table type")));
}

/*
 * Evaluate NextValueExpr.
 */
void
ExecEvalNextValueExpr(ExprState *state, const ExprEvalStep *op, NullableDatum *opres)
{
	int64		newval = nextval_internal(op->d.nextvalueexpr.seqid, false);

	switch (op->d.nextvalueexpr.seqtypid)
	{
		case INT2OID:
			opres->value = Int16GetDatum((int16) newval);
			break;
		case INT4OID:
			opres->value = Int32GetDatum((int32) newval);
			break;
		case INT8OID:
			opres->value = Int64GetDatum((int64) newval);
			break;
		default:
			elog(ERROR, "unsupported sequence type %u",
				 op->d.nextvalueexpr.seqtypid);
	}
	opres->isnull = false;
}

/*
 * Evaluate NullTest / IS NULL for rows.
 */
void
ExecEvalRowNull(ExprState *state, ExprEvalStep *op, ExprContext *econtext,
				NullableDatum *opres)
{
	ExecEvalRowNullInt(state, op, econtext, true, opres);
}

/*
 * Evaluate NullTest / IS NOT NULL for rows.
 */
void
ExecEvalRowNotNull(ExprState *state, ExprEvalStep *op, ExprContext *econtext,
				   NullableDatum *opres)
{
	ExecEvalRowNullInt(state, op, econtext, false, opres);
}

/* Common code for IS [NOT] NULL on a row value */
static void
ExecEvalRowNullInt(ExprState *state, ExprEvalStep *op,
				   ExprContext *econtext, bool checkisnull,
				   NullableDatum *opres)
{
	Datum		value = opres->value;
	bool		isnull = opres->isnull;
	HeapTupleHeader tuple;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupDesc;
	HeapTupleData tmptup;

	opres->isnull = false;

	/* NULL row variables are treated just as NULL scalar columns */
	if (isnull)
	{
		opres->value = BoolGetDatum(checkisnull);
		return;
	}

	/*
	 * The SQL standard defines IS [NOT] NULL for a non-null rowtype argument
	 * as:
	 *
	 * "R IS NULL" is true if every field is the null value.
	 *
	 * "R IS NOT NULL" is true if no field is the null value.
	 *
	 * This definition is (apparently intentionally) not recursive; so our
	 * tests on the fields are primitive attisnull tests, not recursive checks
	 * to see if they are all-nulls or no-nulls rowtypes.
	 *
	 * The standard does not consider the possibility of zero-field rows, but
	 * here we consider them to vacuously satisfy both predicates.
	 */

	tuple = DatumGetHeapTupleHeader(value);

	tupType = HeapTupleHeaderGetTypeId(tuple);
	tupTypmod = HeapTupleHeaderGetTypMod(tuple);

	/* Lookup tupdesc if first time through or if type changes */
	tupDesc = get_cached_rowtype(tupType, tupTypmod,
								 &op->d.nulltest_row.rowcache, NULL);

	/*
	 * heap_attisnull needs a HeapTuple not a bare HeapTupleHeader.
	 */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
	tmptup.t_data = tuple;

	for (int att = 1; att <= tupDesc->natts; att++)
	{
		/* ignore dropped columns */
		if (TupleDescAttr(tupDesc, att - 1)->attisdropped)
			continue;
		if (heap_attisnull(&tmptup, att, tupDesc))
		{
			/* null field disproves IS NOT NULL */
			if (!checkisnull)
			{
				opres->value = BoolGetDatum(false);
				return;
			}
		}
		else
		{
			/* non-null field disproves IS NULL */
			if (checkisnull)
			{
				opres->value = BoolGetDatum(false);
				return;
			}
		}
	}

	opres->value = BoolGetDatum(true);
}

/*
 * Evaluate an ARRAY[] expression.
 *
 * The individual array elements (or subarrays) have already been evaluated
 * into op->d.arrayexpr.elemvalues[]/elemnulls[].
 */
void
ExecEvalArrayExpr(ExprState *state, const ExprEvalStep *op,
				  NullableDatum *opres, NullableDatum *elements)
{
	ArrayType  *result;
	Oid			element_type = op->d.arrayexpr.elemtype;
	int			nelems = op->d.arrayexpr.nelems;
	int			ndims = 0;
	int			dims[MAXDIM];
	int			lbs[MAXDIM];

	/* Set non-null as default */
	opres->isnull = false;

	if (!op->d.arrayexpr.multidims)
	{
		/* Elements are presumably of scalar type */
		// FIXME: better approach?
		Datum	   *dvalues = palloc(sizeof(Datum) * nelems);
		bool	   *dnulls = palloc(sizeof(bool) * nelems);

		for (int i = 0; i < nelems; i++)
		{
			dvalues[i] = elements[i].value;
			dnulls[i] = elements[i].isnull;
		}

		/* setup for 1-D array of the given length */
		ndims = 1;
		dims[0] = nelems;
		lbs[0] = 1;

		result = construct_md_array(dvalues, dnulls, ndims, dims, lbs,
									element_type,
									op->d.arrayexpr.elemlength,
									op->d.arrayexpr.elembyval,
									op->d.arrayexpr.elemalign);

		pfree(dvalues);
		pfree(dnulls);
	}
	else
	{
		/* Must be nested array expressions */
		int			nbytes = 0;
		int			nitems;
		int			outer_nelems = 0;
		int			elem_ndims = 0;
		int		   *elem_dims = NULL;
		int		   *elem_lbs = NULL;
		bool		firstone = true;
		bool		havenulls = false;
		bool		haveempty = false;
		char	  **subdata;
		bits8	  **subbitmaps;
		int		   *subbytes;
		int		   *subnitems;
		int32		dataoffset;
		char	   *dat;
		int			iitem;

		subdata = (char **) palloc(nelems * sizeof(char *));
		subbitmaps = (bits8 **) palloc(nelems * sizeof(bits8 *));
		subbytes = (int *) palloc(nelems * sizeof(int));
		subnitems = (int *) palloc(nelems * sizeof(int));

		/* loop through and get data area from each element */
		for (int elemoff = 0; elemoff < nelems; elemoff++)
		{
			NullableDatum *arrayval;
			ArrayType  *array;
			int			this_ndims;

			arrayval = &elements[elemoff];

			/* temporarily ignore null subarrays */
			if (arrayval->isnull)
			{
				haveempty = true;
				continue;
			}

			array = DatumGetArrayTypeP(arrayval->value);

			/* run-time double-check on element type */
			if (element_type != ARR_ELEMTYPE(array))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("cannot merge incompatible arrays"),
						 errdetail("Array with element type %s cannot be "
								   "included in ARRAY construct with element type %s.",
								   format_type_be(ARR_ELEMTYPE(array)),
								   format_type_be(element_type))));

			this_ndims = ARR_NDIM(array);
			/* temporarily ignore zero-dimensional subarrays */
			if (this_ndims <= 0)
			{
				haveempty = true;
				continue;
			}

			if (firstone)
			{
				/* Get sub-array details from first member */
				elem_ndims = this_ndims;
				ndims = elem_ndims + 1;
				if (ndims <= 0 || ndims > MAXDIM)
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
									ndims, MAXDIM)));

				elem_dims = (int *) palloc(elem_ndims * sizeof(int));
				memcpy(elem_dims, ARR_DIMS(array), elem_ndims * sizeof(int));
				elem_lbs = (int *) palloc(elem_ndims * sizeof(int));
				memcpy(elem_lbs, ARR_LBOUND(array), elem_ndims * sizeof(int));

				firstone = false;
			}
			else
			{
				/* Check other sub-arrays are compatible */
				if (elem_ndims != this_ndims ||
					memcmp(elem_dims, ARR_DIMS(array),
						   elem_ndims * sizeof(int)) != 0 ||
					memcmp(elem_lbs, ARR_LBOUND(array),
						   elem_ndims * sizeof(int)) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
							 errmsg("multidimensional arrays must have array "
									"expressions with matching dimensions")));
			}

			subdata[outer_nelems] = ARR_DATA_PTR(array);
			subbitmaps[outer_nelems] = ARR_NULLBITMAP(array);
			subbytes[outer_nelems] = ARR_SIZE(array) - ARR_DATA_OFFSET(array);
			nbytes += subbytes[outer_nelems];
			/* check for overflow of total request */
			if (!AllocSizeIsValid(nbytes))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("array size exceeds the maximum allowed (%d)",
								(int) MaxAllocSize)));
			subnitems[outer_nelems] = ArrayGetNItems(this_ndims,
													 ARR_DIMS(array));
			havenulls |= ARR_HASNULL(array);
			outer_nelems++;
		}

		/*
		 * If all items were null or empty arrays, return an empty array;
		 * otherwise, if some were and some weren't, raise error.  (Note: we
		 * must special-case this somehow to avoid trying to generate a 1-D
		 * array formed from empty arrays.  It's not ideal...)
		 */
		if (haveempty)
		{
			if (ndims == 0)		/* didn't find any nonempty array */
			{
				opres->value =
					PointerGetDatum(construct_empty_array(element_type));
				return;
			}
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("multidimensional arrays must have array "
							"expressions with matching dimensions")));
		}

		/* setup for multi-D array */
		dims[0] = outer_nelems;
		lbs[0] = 1;
		for (int i = 1; i < ndims; i++)
		{
			dims[i] = elem_dims[i - 1];
			lbs[i] = elem_lbs[i - 1];
		}

		/* check for subscript overflow */
		nitems = ArrayGetNItems(ndims, dims);
		ArrayCheckBounds(ndims, dims, lbs);

		if (havenulls)
		{
			dataoffset = ARR_OVERHEAD_WITHNULLS(ndims, nitems);
			nbytes += dataoffset;
		}
		else
		{
			dataoffset = 0;		/* marker for no null bitmap */
			nbytes += ARR_OVERHEAD_NONULLS(ndims);
		}

		result = (ArrayType *) palloc0(nbytes);
		SET_VARSIZE(result, nbytes);
		result->ndim = ndims;
		result->dataoffset = dataoffset;
		result->elemtype = element_type;
		memcpy(ARR_DIMS(result), dims, ndims * sizeof(int));
		memcpy(ARR_LBOUND(result), lbs, ndims * sizeof(int));

		dat = ARR_DATA_PTR(result);
		iitem = 0;
		for (int i = 0; i < outer_nelems; i++)
		{
			memcpy(dat, subdata[i], subbytes[i]);
			dat += subbytes[i];
			if (havenulls)
				array_bitmap_copy(ARR_NULLBITMAP(result), iitem,
								  subbitmaps[i], 0,
								  subnitems[i]);
			iitem += subnitems[i];
		}
	}

	opres->value = PointerGetDatum(result);
}

void
ExecEvalArrayCoerceRelabel(ExprState *state, const ExprEvalStep *op,
						   NullableDatum *opres)
{
	/* NULL array -> NULL result */
	if (opres->isnull)
		return;
	else
	{
		/* Detoast input array if necessary, and copy in any case */
		Datum		arraydatum = opres->value;
		ArrayType  *array = DatumGetArrayTypePCopy(arraydatum);

		ARR_ELEMTYPE(array) = op->d.arraycoerce.resultelemtype;
		opres->value = PointerGetDatum(array);
		return;
	}
}

/*
 * Evaluate an ArrayCoerceExpr expression.
 *
 * Source array is in step's result variable.
 */
bool
ExecEvalArrayCoerceUnpack(ExprState *state, const ExprEvalStep *op,
						  NullableDatum *opres)
{
	/* NULL array -> NULL result */
	if (opres->isnull)
		return false;

	return array_map_unpack(opres->value,
							op->d.arraycoerce.amstate,
							opres);
}


/*
 * Evaluate an ArrayCoerceExpr expression.
 *
 * Source array is in step's result variable.
 */
bool
ExecEvalArrayCoercePack(ExprState *state, const ExprEvalStep *op,
						NullableDatum *opres)
{
	return array_map_pack(op->d.arraycoerce.resultelemtype,
						  op->d.arraycoerce.amstate,
						  opres);
}

/*
 * Evaluate a ROW() expression.
 *
 * The individual columns have already been evaluated into
 * op->d.row.elemvalues[]/elemnulls[].
 */
void
ExecEvalRow(ExprState *state, const ExprEvalStep *op,
			NullableDatum *opres, NullableDatum *elements)
{
	HeapTuple	tuple;

	/* build tuple from evaluated field values */
	tuple = heap_form_tuple_s(op->d.row.tupdesc,
							  elements);

	opres->value = HeapTupleGetDatum(tuple);
	opres->isnull = false;
}

/*
 * Evaluate GREATEST() or LEAST() expression (note this is *not* MIN()/MAX()).
 *
 * All of the to-be-compared expressions have already been evaluated into
 * op->d.minmax.values[]/nulls[].
 */
void
ExecEvalMinMax(ExprState *state, const ExprEvalStep *op,
			   NullableDatum *opres, NullableDatum *args,
			   FunctionCallInfo fcinfo)
{
	MinMaxOp	operator = op->d.minmax.op;

	/* set at initialization */
	Assert(fcinfo->args[0].isnull == false);
	Assert(fcinfo->args[1].isnull == false);

	/* default to null result */
	opres->isnull = true;

	for (int off = 0; off < op->d.minmax.nelems; off++)
	{
		/* ignore NULL inputs */
		if (args[off].isnull)
			continue;

		if (opres->isnull)
		{
			/* first nonnull input, adopt value */
			*opres = args[off];
		}
		else
		{
			int			cmpresult;

			/* apply comparison function */
			fcinfo->args[0].value = opres->value;
			fcinfo->args[1].value = args[off].value;

			fcinfo->isnull = false;
			cmpresult = DatumGetInt32(op->d.minmax.fn_addr(fcinfo));
			if (fcinfo->isnull) /* probably should not happen */
				continue;

			if (cmpresult > 0 && operator == IS_LEAST)
				opres->value = args[off].value;
			else if (cmpresult < 0 && operator == IS_GREATEST)
				opres->value = args[off].value;
		}
	}
}

/*
 * Evaluate a FieldSelect node.
 *
 * Source record is in step's result variable.
 */
void
ExecEvalFieldSelect(ExprState *state, ExprEvalStep *op, ExprContext *econtext,
					NullableDatum *opres)
{
	AttrNumber	fieldnum = op->d.fieldselect.fieldnum;
	Datum		tupDatum;
	HeapTupleHeader tuple;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupDesc;
	Form_pg_attribute attr;
	HeapTupleData tmptup;

	/* NULL record -> NULL result */
	if (opres->isnull)
		return;

	tupDatum = opres->value;

	/* We can special-case expanded records for speed */
	if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(tupDatum)))
	{
		ExpandedRecordHeader *erh = (ExpandedRecordHeader *) DatumGetEOHP(tupDatum);

		Assert(erh->er_magic == ER_MAGIC);

		/* Extract record's TupleDesc */
		tupDesc = expanded_record_get_tupdesc(erh);

		/*
		 * Find field's attr record.  Note we don't support system columns
		 * here: a datum tuple doesn't have valid values for most of the
		 * interesting system columns anyway.
		 */
		if (fieldnum <= 0)		/* should never happen */
			elog(ERROR, "unsupported reference to system column %d in FieldSelect",
				 fieldnum);
		if (fieldnum > tupDesc->natts)	/* should never happen */
			elog(ERROR, "attribute number %d exceeds number of columns %d",
				 fieldnum, tupDesc->natts);
		attr = TupleDescAttr(tupDesc, fieldnum - 1);

		/* Check for dropped column, and force a NULL result if so */
		if (attr->attisdropped)
		{
			opres->isnull = true;
			return;
		}

		/* Check for type mismatch --- possible after ALTER COLUMN TYPE? */
		/* As in CheckVarSlotCompatibility, we should but can't check typmod */
		if (op->d.fieldselect.resulttype != attr->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("attribute %d has wrong type", fieldnum),
					 errdetail("Table has type %s, but query expects %s.",
							   format_type_be(attr->atttypid),
							   format_type_be(op->d.fieldselect.resulttype))));

		/* extract the field */
		opres->value = expanded_record_get_field(erh, fieldnum,
													  &opres->isnull);
	}
	else
	{
		/* Get the composite datum and extract its type fields */
		tuple = DatumGetHeapTupleHeader(tupDatum);

		tupType = HeapTupleHeaderGetTypeId(tuple);
		tupTypmod = HeapTupleHeaderGetTypMod(tuple);

		/* Lookup tupdesc if first time through or if type changes */
		tupDesc = get_cached_rowtype(tupType, tupTypmod,
									 &op->d.fieldselect.rowcache, NULL);

		/*
		 * Find field's attr record.  Note we don't support system columns
		 * here: a datum tuple doesn't have valid values for most of the
		 * interesting system columns anyway.
		 */
		if (fieldnum <= 0)		/* should never happen */
			elog(ERROR, "unsupported reference to system column %d in FieldSelect",
				 fieldnum);
		if (fieldnum > tupDesc->natts)	/* should never happen */
			elog(ERROR, "attribute number %d exceeds number of columns %d",
				 fieldnum, tupDesc->natts);
		attr = TupleDescAttr(tupDesc, fieldnum - 1);

		/* Check for dropped column, and force a NULL result if so */
		if (attr->attisdropped)
		{
			opres->isnull = true;
			return;
		}

		/* Check for type mismatch --- possible after ALTER COLUMN TYPE? */
		/* As in CheckVarSlotCompatibility, we should but can't check typmod */
		if (op->d.fieldselect.resulttype != attr->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("attribute %d has wrong type", fieldnum),
					 errdetail("Table has type %s, but query expects %s.",
							   format_type_be(attr->atttypid),
							   format_type_be(op->d.fieldselect.resulttype))));

		/* heap_getattr needs a HeapTuple not a bare HeapTupleHeader */
		tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
		tmptup.t_data = tuple;

		/* extract the field */
		opres->value = heap_getattr(&tmptup,
										 fieldnum,
										 tupDesc,
										 &opres->isnull);
	}
}

/*
 * Deform source tuple, filling in the step's values/nulls arrays, before
 * evaluating individual new values as part of a FieldStore expression.
 * Subsequent steps will overwrite individual elements of the values/nulls
 * arrays with the new field values, and then FIELDSTORE_FORM will build the
 * new tuple value.
 *
 * Source record is in step's result variable.
 */
void
ExecEvalFieldStoreDeForm(ExprState *state, const ExprEvalStep *op, ExprContext *econtext,
						 NullableDatum *opres, NullableDatum *columns)
{
	TupleDesc	tupDesc;

	/* Lookup tupdesc if first time through or if type changes */
	tupDesc = get_cached_rowtype(op->d.fieldstore.fstore->resulttype, -1,
								 op->d.fieldstore.rowcache, NULL);

	/* Check that current tupdesc doesn't have more fields than we allocated */
	if (unlikely(tupDesc->natts > op->d.fieldstore.ncolumns))
		elog(ERROR, "too many columns in composite type %u",
			 op->d.fieldstore.fstore->resulttype);

	if (opres->isnull)
	{
		/* Convert null input tuple into an all-nulls row */
		for (int i = 0; i < op->d.fieldstore.ncolumns; i++)
			columns[i].isnull = true;
	}
	else
	{
		/*
		 * heap_deform_tuple needs a HeapTuple not a bare HeapTupleHeader. We
		 * set all the fields in the struct just in case.
		 */
		Datum		tupDatum = opres->value;
		HeapTupleHeader tuphdr;
		HeapTupleData tmptup;

		tuphdr = DatumGetHeapTupleHeader(tupDatum);
		tmptup.t_len = HeapTupleHeaderGetDatumLength(tuphdr);
		ItemPointerSetInvalid(&(tmptup.t_self));
		tmptup.t_tableOid = InvalidOid;
		tmptup.t_data = tuphdr;

		heap_deform_tuple_s(&tmptup, tupDesc, columns);
	}
}

/*
 * Compute the new composite datum after each individual field value of a
 * FieldStore expression has been evaluated.
 */
void
ExecEvalFieldStoreForm(ExprState *state, const ExprEvalStep *op,
					   NullableDatum *opres, NullableDatum *columns)
{
	TupleDesc	tupDesc;
	HeapTuple	tuple;

	/* Lookup tupdesc (should be valid already) */
	tupDesc = get_cached_rowtype(op->d.fieldstore.fstore->resulttype, -1,
								 op->d.fieldstore.rowcache, NULL);
	tuple = heap_form_tuple_s(tupDesc, columns);

	opres->value = HeapTupleGetDatum(tuple);
	opres->isnull = false;
}

/*
 * Evaluate a rowtype coercion operation.
 * This may require rearranging field positions.
 *
 * Source record is in step's result variable.
 */
void
ExecEvalConvertRowtype(ExprState *state, ExprEvalStep *op, ExprContext *econtext,
					   NullableDatum *opres)
{
	HeapTuple	result;
	HeapTupleHeader tuple;
	HeapTupleData tmptup;
	TupleDesc	indesc,
				outdesc;
	bool		changed = false;

	/* NULL in -> NULL out */
	if (opres->isnull)
		return;

	tuple = DatumGetHeapTupleHeader(opres->value);

	/*
	 * Lookup tupdescs if first time through or if type changes.  We'd better
	 * pin them since type conversion functions could do catalog lookups and
	 * hence cause cache invalidation.
	 */
	indesc = get_cached_rowtype(op->d.convert_rowtype.inputtype, -1,
								op->d.convert_rowtype.incache,
								&changed);
	IncrTupleDescRefCount(indesc);
	outdesc = get_cached_rowtype(op->d.convert_rowtype.outputtype, -1,
								 op->d.convert_rowtype.outcache,
								 &changed);
	IncrTupleDescRefCount(outdesc);

	/*
	 * We used to be able to assert that incoming tuples are marked with
	 * exactly the rowtype of indesc.  However, now that ExecEvalWholeRowVar
	 * might change the tuples' marking to plain RECORD due to inserting
	 * aliases, we can only make this weak test:
	 */
	Assert(HeapTupleHeaderGetTypeId(tuple) == indesc->tdtypeid ||
		   HeapTupleHeaderGetTypeId(tuple) == RECORDOID);

	/* if first time through, or after change, initialize conversion map */
	if (changed)
	{
		MemoryContext old_cxt;

		/* allocate map in long-lived memory context */
		old_cxt = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);

		/* prepare map from old to new attribute numbers */
		op->d.convert_rowtype.map = convert_tuples_by_name(indesc, outdesc);

		MemoryContextSwitchTo(old_cxt);
	}

	/* Following steps need a HeapTuple not a bare HeapTupleHeader */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
	tmptup.t_data = tuple;

	if (op->d.convert_rowtype.map != NULL)
	{
		/* Full conversion with attribute rearrangement needed */
		result = execute_attr_map_tuple(&tmptup, op->d.convert_rowtype.map);
		/* Result already has appropriate composite-datum header fields */
		opres->value = HeapTupleGetDatum(result);
	}
	else
	{
		/*
		 * The tuple is physically compatible as-is, but we need to insert the
		 * destination rowtype OID in its composite-datum header field, so we
		 * have to copy it anyway.  heap_copy_tuple_as_datum() is convenient
		 * for this since it will both make the physical copy and insert the
		 * correct composite header fields.  Note that we aren't expecting to
		 * have to flatten any toasted fields: the input was a composite
		 * datum, so it shouldn't contain any.  So heap_copy_tuple_as_datum()
		 * is overkill here, but its check for external fields is cheap.
		 */
		opres->value = heap_copy_tuple_as_datum(&tmptup, outdesc);
	}

	DecrTupleDescRefCount(indesc);
	DecrTupleDescRefCount(outdesc);
}

/*
 * Evaluate "scalar op ANY/ALL (array)".
 *
 * Source array is in our result area, scalar arg is already evaluated into
 * fcinfo->args[0].
 *
 * The operator always yields boolean, and we combine the results across all
 * array elements using OR and AND (for ANY and ALL respectively).  Of course
 * we short-circuit as soon as the result is known.
 */
void
ExecEvalScalarArrayOp(ExprState *state, ExprEvalStep *op,
					  NullableDatum *opres, FunctionCallInfo fcinfo)
{
	bool		useOr = op->d.scalararrayop.useOr;
	bool		strictfunc = op->d.scalararrayop.finfo->fn_strict;
	ArrayType  *arr;
	int			nitems;
	Datum		result;
	bool		resultnull;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	char	   *s;
	bits8	   *bitmap;
	int			bitmask;

	/*
	 * If the array is NULL then we return NULL --- it's not very meaningful
	 * to do anything else, even if the operator isn't strict.
	 */
	if (opres->isnull)
		return;

	/* Else okay to fetch and detoast the array */
	arr = DatumGetArrayTypeP(opres->value);

	/*
	 * If the array is empty, we return either FALSE or TRUE per the useOr
	 * flag.  This is correct even if the scalar is NULL; since we would
	 * evaluate the operator zero times, it matters not whether it would want
	 * to return NULL.
	 */
	nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
	if (nitems <= 0)
	{
		opres->value = BoolGetDatum(!useOr);
		opres->isnull = false;
		return;
	}

	/*
	 * If the scalar is NULL, and the function is strict, return NULL; no
	 * point in iterating the loop.
	 */
	if (fcinfo->args[0].isnull && strictfunc)
	{
		opres->isnull = true;
		return;
	}

	/*
	 * We arrange to look up info about the element type only once per series
	 * of calls, assuming the element type doesn't change underneath us.
	 */
	if (op->d.scalararrayop.element_type != ARR_ELEMTYPE(arr))
	{
		get_typlenbyvalalign(ARR_ELEMTYPE(arr),
							 &op->d.scalararrayop.typlen,
							 &op->d.scalararrayop.typbyval,
							 &op->d.scalararrayop.typalign);
		op->d.scalararrayop.element_type = ARR_ELEMTYPE(arr);
	}

	typlen = op->d.scalararrayop.typlen;
	typbyval = op->d.scalararrayop.typbyval;
	typalign = op->d.scalararrayop.typalign;

	/* Initialize result appropriately depending on useOr */
	result = BoolGetDatum(!useOr);
	resultnull = false;

	/* Loop over the array elements */
	s = (char *) ARR_DATA_PTR(arr);
	bitmap = ARR_NULLBITMAP(arr);
	bitmask = 1;

	for (int i = 0; i < nitems; i++)
	{
		Datum		elt;
		Datum		thisresult;

		/* Get array element, checking for NULL */
		if (bitmap && (*bitmap & bitmask) == 0)
		{
			fcinfo->args[1].value = (Datum) 0;
			fcinfo->args[1].isnull = true;
		}
		else
		{
			elt = fetch_att(s, typbyval, typlen);
			s = att_addlength_pointer(s, typlen, s);
			s = (char *) att_align_nominal(s, typalign);
			fcinfo->args[1].value = elt;
			fcinfo->args[1].isnull = false;
		}

		/* Call comparison function */
		if (fcinfo->args[1].isnull && strictfunc)
		{
			fcinfo->isnull = true;
			thisresult = (Datum) 0;
		}
		else
		{
			fcinfo->isnull = false;
			thisresult = op->d.scalararrayop.finfo->fn_addr(fcinfo);
		}

		/* Combine results per OR or AND semantics */
		if (fcinfo->isnull)
			resultnull = true;
		else if (useOr)
		{
			if (DatumGetBool(thisresult))
			{
				result = BoolGetDatum(true);
				resultnull = false;
				break;			/* needn't look at any more elements */
			}
		}
		else
		{
			if (!DatumGetBool(thisresult))
			{
				result = BoolGetDatum(false);
				resultnull = false;
				break;			/* needn't look at any more elements */
			}
		}

		/* advance bitmap pointer if any */
		if (bitmap)
		{
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				bitmap++;
				bitmask = 1;
			}
		}
	}

	opres->value = result;
	opres->isnull = resultnull;
}

/*
 * Hash function for scalar array hash op elements.
 *
 * We use the element type's default hash opclass, and the column collation
 * if the type is collation-sensitive.
 */
static uint32
saop_element_hash(struct saophash_hash *tb, Datum key)
{
	ScalarArrayOpExprHashTable *elements_tab = (ScalarArrayOpExprHashTable *) tb->private_data;
	FunctionCallInfo fcinfo = &elements_tab->hash_fcinfo_data;
	Datum		hash;

	fcinfo->args[0].value = key;
	fcinfo->args[0].isnull = false;

	hash = elements_tab->hash_finfo.fn_addr(fcinfo);

	return DatumGetUInt32(hash);
}

/*
 * Matching function for scalar array hash op elements, to be used in hashtable
 * lookups.
 */
static bool
saop_hash_element_match(struct saophash_hash *tb, Datum key1, Datum key2)
{
	Datum		result;

	ScalarArrayOpExprHashTable *elements_tab = (ScalarArrayOpExprHashTable *) tb->private_data;
	FunctionCallInfo fcinfo = elements_tab->match_fcinfo;

	fcinfo->args[0].value = key1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = key2;
	fcinfo->args[1].isnull = false;

	/* XXX FIXME */
	result = elements_tab->match_fn_addr(fcinfo);

	return DatumGetBool(result);
}

/*
 * Evaluate "scalar op ANY (const array)".
 *
 * Similar to ExecEvalScalarArrayOp, but optimized for faster repeat lookups
 * by building a hashtable on the first lookup.  This hashtable will be reused
 * by subsequent lookups.  Unlike ExecEvalScalarArrayOp, this version only
 * supports OR semantics.
 *
 * Source array is in our result area, scalar arg is already evaluated into
 * fcinfo->args[0].
 *
 * The operator always yields boolean.
 */
void
ExecEvalHashedScalarArrayOp(ExprState *state, ExprEvalStep *op, ExprContext *econtext,
							NullableDatum *opres, FunctionCallInfo fcinfo)
{
	ScalarArrayOpExprHashTable *elements_tab = op->d.hashedscalararrayop.elements_tab;
	bool		inclause = op->d.hashedscalararrayop.inclause;
	bool		strictfunc = op->d.hashedscalararrayop.finfo->fn_strict;
	Datum		scalar = fcinfo->args[0].value;
	bool		scalar_isnull = fcinfo->args[0].isnull;
	Datum		result;
	bool		resultnull;
	bool		hashfound;

	/* We don't setup a hashed scalar array op if the array const is null. */
	Assert(!opres->isnull);

	/*
	 * If the scalar is NULL, and the function is strict, return NULL; no
	 * point in executing the search.
	 */
	if (fcinfo->args[0].isnull && strictfunc)
	{
		opres->isnull = true;
		return;
	}

	/* Build the hash table on first evaluation */
	if (elements_tab == NULL)
	{
		ScalarArrayOpExpr *saop;
		int16		typlen;
		bool		typbyval;
		char		typalign;
		int			nitems;
		bool		has_nulls = false;
		char	   *s;
		bits8	   *bitmap;
		int			bitmask;
		MemoryContext oldcontext;
		ArrayType  *arr;

		saop = op->d.hashedscalararrayop.saop;

		arr = DatumGetArrayTypeP(opres->value);
		nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));

		get_typlenbyvalalign(ARR_ELEMTYPE(arr),
							 &typlen,
							 &typbyval,
							 &typalign);

		oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);

		elements_tab = (ScalarArrayOpExprHashTable *)
			palloc0(offsetof(ScalarArrayOpExprHashTable, hash_fcinfo_data) +
					SizeForFunctionCallInfo(1));
		op->d.hashedscalararrayop.elements_tab = elements_tab;

		elements_tab->match_fn_addr = op->d.hashedscalararrayop.finfo->fn_addr;
		elements_tab->match_fcinfo = fcinfo;

		fmgr_info(saop->hashfuncid, &elements_tab->hash_finfo);
		fmgr_info_set_expr((Node *) saop, &elements_tab->hash_finfo);

		InitFunctionCallInfoData(elements_tab->hash_fcinfo_data,
								 &elements_tab->hash_finfo,
								 1,
								 saop->inputcollid,
								 NULL,
								 NULL);

		/*
		 * Create the hash table sizing it according to the number of elements
		 * in the array.  This does assume that the array has no duplicates.
		 * If the array happens to contain many duplicate values then it'll
		 * just mean that we sized the table a bit on the large side.
		 */
		elements_tab->hashtab = saophash_create(CurrentMemoryContext, nitems,
												elements_tab);

		MemoryContextSwitchTo(oldcontext);

		s = (char *) ARR_DATA_PTR(arr);
		bitmap = ARR_NULLBITMAP(arr);
		bitmask = 1;
		for (int i = 0; i < nitems; i++)
		{
			/* Get array element, checking for NULL. */
			if (bitmap && (*bitmap & bitmask) == 0)
			{
				has_nulls = true;
			}
			else
			{
				Datum		element;

				element = fetch_att(s, typbyval, typlen);
				s = att_addlength_pointer(s, typlen, s);
				s = (char *) att_align_nominal(s, typalign);

				saophash_insert(elements_tab->hashtab, element, &hashfound);
			}

			/* Advance bitmap pointer if any. */
			if (bitmap)
			{
				bitmask <<= 1;
				if (bitmask == 0x100)
				{
					bitmap++;
					bitmask = 1;
				}
			}
		}

		/*
		 * Remember if we had any nulls so that we know if we need to execute
		 * non-strict functions with a null lhs value if no match is found.
		 */
		op->d.hashedscalararrayop.has_nulls = has_nulls;
	}
	else
	{
#if 0
		/* FIXME TODO: handle these */
		Assert(elements_tab->fn_addr == op->d.hashedscalararrayop.fn_addr);
		Assert(elements_tab->hash_fn_addr == op->d.hashedscalararrayop.hash_fn_addr);
		Assert(elements_tab->hash_fcinfo == op->d.hashedscalararrayop.hash_fcinfo_data);
#endif
		/* fcinfo may be allocated on stack, so can't assume it stays constant */
		elements_tab->match_fcinfo = fcinfo;
	}

	/* Check the hash to see if we have a match. */
	hashfound = NULL != saophash_lookup(elements_tab->hashtab, scalar);

	/* the result depends on if the clause is an IN or NOT IN clause */
	if (inclause)
		result = BoolGetDatum(hashfound);	/* IN */
	else
		result = BoolGetDatum(!hashfound);	/* NOT IN */

	resultnull = false;

	/*
	 * If we didn't find a match in the array, we still might need to handle
	 * the possibility of null values.  We didn't put any NULLs into the
	 * hashtable, but instead marked if we found any when building the table
	 * in has_nulls.
	 */
	if (!hashfound && op->d.hashedscalararrayop.has_nulls)
	{
		if (strictfunc)
		{

			/*
			 * We have nulls in the array so a non-null lhs and no match must
			 * yield NULL.
			 */
			result = (Datum) 0;
			resultnull = true;
		}
		else
		{
			/*
			 * Execute function will null rhs just once.
			 *
			 * The hash lookup path will have scribbled on the lhs argument so
			 * we need to set it up also (even though we entered this function
			 * with it already set).
			 */
			fcinfo->args[0].value = scalar;
			fcinfo->args[0].isnull = scalar_isnull;
			fcinfo->args[1].value = (Datum) 0;
			fcinfo->args[1].isnull = true;

			result = op->d.hashedscalararrayop.finfo->fn_addr(fcinfo);
			resultnull = fcinfo->isnull;

			/*
			 * Reverse the result for NOT IN clauses since the above function
			 * is the equality function and we need not-equals.
			 */
			if (!inclause)
				result = !result;
		}
	}

	opres->value = result;
	opres->isnull = resultnull;
}

/*
 * Evaluate a NOT NULL domain constraint.
 */
void
ExecEvalConstraintNotNull(ExprState *state, const ExprEvalStep *op, NullableDatum *opres)
{
	if (opres->isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NOT_NULL_VIOLATION),
				 errmsg("domain %s does not allow null values",
						format_type_be(op->d.domaincheck.resulttype)),
				 errdatatype(op->d.domaincheck.resulttype)));
}

/*
 * Evaluate a CHECK domain constraint.
 */
void
ExecEvalConstraintCheck(ExprState *state, const ExprEvalStep *op, NullableDatum *check)
{
	if (!check->isnull &&
		!DatumGetBool(check->value))
		ereport(ERROR,
				(errcode(ERRCODE_CHECK_VIOLATION),
				 errmsg("value for domain %s violates check constraint \"%s\"",
						format_type_be(op->d.domaincheck.resulttype),
						op->d.domaincheck.constraintname),
				 errdomainconstraint(op->d.domaincheck.resulttype,
									 op->d.domaincheck.constraintname)));
}

/*
 * Evaluate the various forms of XmlExpr.
 *
 * Arguments have been evaluated into named_argvalue/named_argnull
 * and/or argvalue/argnull arrays.
 */
void
ExecEvalXmlExpr(ExprState *state, const ExprEvalStep *op,
				NullableDatum *opres, NullableDatum *args,
				NullableDatum *named_args)
{
	XmlExpr    *xexpr = op->d.xmlexpr.xexpr;
	Datum		value;

	*opres = NULL_DATUM;	/* until we get a result */

	switch (xexpr->op)
	{
		case IS_XMLCONCAT:
			{
				List	   *values = NIL;

				for (int i = 0; i < list_length(xexpr->args); i++)
				{
					if (!args[i].isnull)
						values = lappend(values, DatumGetPointer(args[i].value));
				}

				if (values != NIL)
				{
					opres->value = PointerGetDatum(xmlconcat(values));
					opres->isnull = false;
				}
			}
			break;

		case IS_XMLFOREST:
			{
				StringInfoData buf;
				ListCell   *lc;
				ListCell   *lc2;
				int			i;

				initStringInfo(&buf);

				i = 0;
				forboth(lc, xexpr->named_args, lc2, xexpr->arg_names)
				{
					Expr	   *e = (Expr *) lfirst(lc);
					char	   *argname = strVal(lfirst(lc2));

					if (!named_args[i].isnull)
					{
						value = named_args[i].value;
						appendStringInfo(&buf, "<%s>%s</%s>",
										 argname,
										 map_sql_value_to_xml_value(value,
																	exprType((Node *) e), true),
										 argname);
						opres->isnull = false;
					}
					i++;
				}

				if (!opres->isnull)
				{
					text	   *result;

					result = cstring_to_text_with_len(buf.data, buf.len);
					opres->value = PointerGetDatum(result);
				}

				pfree(buf.data);
			}
			break;

		case IS_XMLELEMENT:
			opres->value =
				PointerGetDatum(xmlelement(xexpr, named_args, args));
			opres->isnull = false;
			break;

		case IS_XMLPARSE:
			{
				text	   *data;
				bool		preserve_whitespace;

				/* arguments are known to be text, bool */
				Assert(list_length(xexpr->args) == 2);

				if (args[0].isnull)
					return;
				value = args[0].value;
				data = DatumGetTextPP(value);

				if (args[1].isnull) /* probably can't happen */
					return;
				value = args[1].value;
				preserve_whitespace = DatumGetBool(value);

				opres->value = PointerGetDatum(
					xmlparse(data,
							 xexpr->xmloption,
							 preserve_whitespace));
				opres->isnull = false;
			}
			break;

		case IS_XMLPI:
			{
				text	   *arg;
				bool		isnull;

				/* optional argument is known to be text */
				Assert(list_length(xexpr->args) <= 1);

				if (xexpr->args)
				{
					isnull = args[0].isnull;
					if (isnull)
						arg = NULL;
					else
						arg = DatumGetTextPP(args[0].value);
				}
				else
				{
					arg = NULL;
					isnull = false;
				}

				opres->value = PointerGetDatum(
					xmlpi(xexpr->name,
						  arg,
						  isnull,
						  &opres->isnull));
			}
			break;

		case IS_XMLROOT:
			{
				xmltype    *data;
				text	   *version;
				int			standalone;

				/* arguments are known to be xml, text, int */
				Assert(list_length(xexpr->args) == 3);

				if (args[0].isnull)
					return;
				data = DatumGetXmlP(args[0].value);

				if (args[1].isnull)
					version = NULL;
				else
					version = DatumGetTextPP(args[1].value);

				Assert(!args[2].isnull);	/* always present */
				standalone = DatumGetInt32(args[2].value);

				opres->value = PointerGetDatum(
					xmlroot(data,
							version,
							standalone));
				opres->isnull = false;
			}
			break;

		case IS_XMLSERIALIZE:
			{
				/* argument type is known to be xml */
				Assert(list_length(xexpr->args) == 1);

				if (args[0].isnull)
					return;
				value = args[0].value;

				opres->value =
					PointerGetDatum(xmltotext_with_options(DatumGetXmlP(value),
														   xexpr->xmloption,
														   xexpr->indent));
				opres->isnull = false;
			}
			break;

		case IS_DOCUMENT:
			{
				/* optional argument is known to be xml */
				Assert(list_length(xexpr->args) == 1);

				if (args[0].isnull)
					return;
				value = args[0].value;

				opres->value =
					BoolGetDatum(xml_is_document(DatumGetXmlP(value)));
				opres->isnull = false;
			}
			break;

		default:
			elog(ERROR, "unrecognized XML operation");
			break;
	}
}

/*
 * Evaluate a JSON constructor expression.
 */
void
ExecEvalJsonConstructor(ExprState *state, const ExprEvalStep *op,
						ExprContext *econtext, NullableDatum *opres,
						NullableDatum *arg_values)
{
	Datum		res;
	JsonConstructorExprState *jcstate = op->d.json_constructor.jcstate;
	JsonConstructorExpr *ctor = jcstate->constructor;
	bool		is_jsonb = ctor->returning->format->format_type == JS_FORMAT_JSONB;
	bool		isnull = false;

	if (ctor->type == JSCTOR_JSON_ARRAY)
		res = (is_jsonb ?
			   jsonb_build_array_worker :
			   json_build_array_worker) (jcstate->nargs,
										 arg_values, jcstate->arg_types,
										 jcstate->constructor->absent_on_null);
	else if (ctor->type == JSCTOR_JSON_OBJECT)
		res = (is_jsonb ?
			   jsonb_build_object_worker :
			   json_build_object_worker) (jcstate->nargs,
										  arg_values, jcstate->arg_types,
										  jcstate->constructor->absent_on_null,
										  jcstate->constructor->unique);
	else if (ctor->type == JSCTOR_JSON_SCALAR)
	{
		if (arg_values[0].isnull)
		{
			res = (Datum) 0;
			isnull = true;
		}
		else
		{
			Datum		value = arg_values[0].value;
			Oid			outfuncid = jcstate->arg_type_cache[0].outfuncid;
			JsonTypeCategory category = (JsonTypeCategory)
				jcstate->arg_type_cache[0].category;

			if (is_jsonb)
				res = datum_to_jsonb(value, category, outfuncid);
			else
				res = datum_to_json(value, category, outfuncid);
		}
	}
	else if (ctor->type == JSCTOR_JSON_PARSE)
	{
		if (arg_values[0].isnull)
		{
			res = (Datum) 0;
			isnull = true;
		}
		else
		{
			Datum		value = arg_values[0].value;
			text	   *js = DatumGetTextP(value);

			if (is_jsonb)
				res = jsonb_from_text(js, true);
			else
			{
				(void) json_validate(js, true, true);
				res = value;
			}
		}
	}
	else
		elog(ERROR, "invalid JsonConstructorExpr type %d", ctor->type);

	opres->value = res;
	opres->isnull = isnull;
}

/*
 * Evaluate a IS JSON predicate.
 */
void
ExecEvalJsonIsPredicate(ExprState *state, const ExprEvalStep *op,
						NullableDatum *opres)
{
	JsonIsPredicate *pred = op->d.is_json.pred;
	Datum		js = opres->value;
	Oid			exprtype;
	bool		res;

	if (opres->isnull)
	{
		opres->value = BoolGetDatum(false);
		return;
	}

	exprtype = exprType(pred->expr);

	if (exprtype == TEXTOID || exprtype == JSONOID)
	{
		text	   *json = DatumGetTextP(js);

		if (pred->item_type == JS_TYPE_ANY)
			res = true;
		else
		{
			switch (json_get_first_token(json, false))
			{
				case JSON_TOKEN_OBJECT_START:
					res = pred->item_type == JS_TYPE_OBJECT;
					break;
				case JSON_TOKEN_ARRAY_START:
					res = pred->item_type == JS_TYPE_ARRAY;
					break;
				case JSON_TOKEN_STRING:
				case JSON_TOKEN_NUMBER:
				case JSON_TOKEN_TRUE:
				case JSON_TOKEN_FALSE:
				case JSON_TOKEN_NULL:
					res = pred->item_type == JS_TYPE_SCALAR;
					break;
				default:
					res = false;
					break;
			}
		}

		/*
		 * Do full parsing pass only for uniqueness check or for JSON text
		 * validation.
		 */
		if (res && (pred->unique_keys || exprtype == TEXTOID))
			res = json_validate(json, pred->unique_keys, false);
	}
	else if (exprtype == JSONBOID)
	{
		if (pred->item_type == JS_TYPE_ANY)
			res = true;
		else
		{
			Jsonb	   *jb = DatumGetJsonbP(js);

			switch (pred->item_type)
			{
				case JS_TYPE_OBJECT:
					res = JB_ROOT_IS_OBJECT(jb);
					break;
				case JS_TYPE_ARRAY:
					res = JB_ROOT_IS_ARRAY(jb) && !JB_ROOT_IS_SCALAR(jb);
					break;
				case JS_TYPE_SCALAR:
					res = JB_ROOT_IS_ARRAY(jb) && JB_ROOT_IS_SCALAR(jb);
					break;
				default:
					res = false;
					break;
			}
		}

		/* Key uniqueness check is redundant for jsonb */
	}
	else
		res = false;

	opres->value = BoolGetDatum(res);
}


/*
 * ExecEvalGroupingFunc
 *
 * Computes a bitmask with a bit for each (unevaluated) argument expression
 * (rightmost arg is least significant bit).
 *
 * A bit is set if the corresponding expression is NOT part of the set of
 * grouping expressions in the current grouping set.
 */
void
ExecEvalGroupingFunc(ExprState *state, const ExprEvalStep *op,
					 NullableDatum *opres)
{
	AggState   *aggstate = castNode(AggState, state->parent);
	int			result = 0;
	Bitmapset  *grouped_cols = aggstate->grouped_cols;
	ListCell   *lc;

	foreach(lc, op->d.grouping_func.clauses)
	{
		int			attnum = lfirst_int(lc);

		result <<= 1;

		if (!bms_is_member(attnum, grouped_cols))
			result |= 1;
	}

	opres->value = Int32GetDatum(result);
	opres->isnull = false;
}

/*
 * Hand off evaluation of a subplan to nodeSubplan.c
 */
void
ExecEvalSubPlan(ExprState *state, const ExprEvalStep *op, ExprContext *econtext,
				NullableDatum *opres)
{
	SubPlanState *sstate = op->d.subplan.sstate;

	/* could potentially be nested, so make sure there's enough stack */
	check_stack_depth();

	opres->value =
		ExecSubPlan(sstate, econtext, &opres->isnull);
}

/*
 * Evaluate a wholerow Var expression.
 *
 * Returns a Datum whose value is the value of a whole-row range variable
 * with respect to given expression context.
 */
void
ExecEvalWholeRowVar(ExprState *state, ExprEvalStep *op, ExprContext *econtext,
					NullableDatum *opres)
{
	Var		   *variable = op->d.wholerow.var;
	TupleTableSlot *slot;
	TupleDesc	output_tupdesc;
	MemoryContext oldcontext;
	HeapTupleHeader dtuple;
	HeapTuple	tuple;

	/* This was checked by ExecInitExpr */
	Assert(variable->varattno == InvalidAttrNumber);

	/* Get the input slot we want */
	switch (variable->varno)
	{
		case INNER_VAR:
			/* get the tuple from the inner node */
			slot = econtext->ecxt_innertuple;
			break;

		case OUTER_VAR:
			/* get the tuple from the outer node */
			slot = econtext->ecxt_outertuple;
			break;

			/* INDEX_VAR is handled by default case */

		default:
			/* get the tuple from the relation being scanned */
			slot = econtext->ecxt_scantuple;
			break;
	}

	/* Apply the junkfilter if any */
	if (op->d.wholerow.junkFilter != NULL)
		slot = ExecFilterJunk(op->d.wholerow.junkFilter, slot);

	/*
	 * If first time through, obtain tuple descriptor and check compatibility.
	 *
	 * XXX: It'd be great if this could be moved to the expression
	 * initialization phase, but due to using slots that's currently not
	 * feasible.
	 */
	if (op->d.wholerow.first)
	{
		/* optimistically assume we don't need slow path */
		op->d.wholerow.slow = false;

		/*
		 * If the Var identifies a named composite type, we must check that
		 * the actual tuple type is compatible with it.
		 */
		if (variable->vartype != RECORDOID)
		{
			TupleDesc	var_tupdesc;
			TupleDesc	slot_tupdesc;

			/*
			 * We really only care about numbers of attributes and data types.
			 * Also, we can ignore type mismatch on columns that are dropped
			 * in the destination type, so long as (1) the physical storage
			 * matches or (2) the actual column value is NULL.  Case (1) is
			 * helpful in some cases involving out-of-date cached plans, while
			 * case (2) is expected behavior in situations such as an INSERT
			 * into a table with dropped columns (the planner typically
			 * generates an INT4 NULL regardless of the dropped column type).
			 * If we find a dropped column and cannot verify that case (1)
			 * holds, we have to use the slow path to check (2) for each row.
			 *
			 * If vartype is a domain over composite, just look through that
			 * to the base composite type.
			 */
			var_tupdesc = lookup_rowtype_tupdesc_domain(variable->vartype,
														-1, false);

			slot_tupdesc = slot->tts_tupleDescriptor;

			if (var_tupdesc->natts != slot_tupdesc->natts)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("table row type and query-specified row type do not match"),
						 errdetail_plural("Table row contains %d attribute, but query expects %d.",
										  "Table row contains %d attributes, but query expects %d.",
										  slot_tupdesc->natts,
										  slot_tupdesc->natts,
										  var_tupdesc->natts)));

			for (int i = 0; i < var_tupdesc->natts; i++)
			{
				Form_pg_attribute vattr = TupleDescAttr(var_tupdesc, i);
				Form_pg_attribute sattr = TupleDescAttr(slot_tupdesc, i);

				if (vattr->atttypid == sattr->atttypid)
					continue;	/* no worries */
				if (!vattr->attisdropped)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("table row type and query-specified row type do not match"),
							 errdetail("Table has type %s at ordinal position %d, but query expects %s.",
									   format_type_be(sattr->atttypid),
									   i + 1,
									   format_type_be(vattr->atttypid))));

				if (vattr->attlen != sattr->attlen ||
					vattr->attalign != sattr->attalign)
					op->d.wholerow.slow = true; /* need to check for nulls */
			}

			/*
			 * Use the variable's declared rowtype as the descriptor for the
			 * output values.  In particular, we *must* absorb any
			 * attisdropped markings.
			 */
			oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
			output_tupdesc = CreateTupleDescCopy(var_tupdesc);
			MemoryContextSwitchTo(oldcontext);

			ReleaseTupleDesc(var_tupdesc);
		}
		else
		{
			/*
			 * In the RECORD case, we use the input slot's rowtype as the
			 * descriptor for the output values, modulo possibly assigning new
			 * column names below.
			 */
			oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
			output_tupdesc = CreateTupleDescCopy(slot->tts_tupleDescriptor);
			MemoryContextSwitchTo(oldcontext);

			/*
			 * It's possible that the input slot is a relation scan slot and
			 * so is marked with that relation's rowtype.  But we're supposed
			 * to be returning RECORD, so reset to that.
			 */
			output_tupdesc->tdtypeid = RECORDOID;
			output_tupdesc->tdtypmod = -1;

			/*
			 * We already got the correct physical datatype info above, but
			 * now we should try to find the source RTE and adopt its column
			 * aliases, since it's unlikely that the input slot has the
			 * desired names.
			 *
			 * If we can't locate the RTE, assume the column names we've got
			 * are OK.  (As of this writing, the only cases where we can't
			 * locate the RTE are in execution of trigger WHEN clauses, and
			 * then the Var will have the trigger's relation's rowtype, so its
			 * names are fine.)  Also, if the creator of the RTE didn't bother
			 * to fill in an eref field, assume our column names are OK. (This
			 * happens in COPY, and perhaps other places.)
			 */
			if (econtext->ecxt_estate &&
				variable->varno <= econtext->ecxt_estate->es_range_table_size)
			{
				RangeTblEntry *rte = exec_rt_fetch(variable->varno,
												   econtext->ecxt_estate);

				if (rte->eref)
					ExecTypeSetColNames(output_tupdesc, rte->eref->colnames);
			}
		}

		/* Bless the tupdesc if needed, and save it in the execution state */
		op->d.wholerow.tupdesc = BlessTupleDesc(output_tupdesc);

		op->d.wholerow.first = false;
	}

	/*
	 * Make sure all columns of the slot are accessible in the slot's
	 * Datum/isnull arrays.
	 */
	slot_getallattrs(slot);

	if (op->d.wholerow.slow)
	{
		/* Check to see if any dropped attributes are non-null */
		TupleDesc	tupleDesc = slot->tts_tupleDescriptor;
		TupleDesc	var_tupdesc = op->d.wholerow.tupdesc;

		Assert(var_tupdesc->natts == tupleDesc->natts);

		for (int i = 0; i < var_tupdesc->natts; i++)
		{
			Form_pg_attribute vattr = TupleDescAttr(var_tupdesc, i);
			Form_pg_attribute sattr = TupleDescAttr(tupleDesc, i);

			if (!vattr->attisdropped)
				continue;		/* already checked non-dropped cols */
			if (slot->tts_values[i].isnull)
				continue;		/* null is always okay */
			if (vattr->attlen != sattr->attlen ||
				vattr->attalign != sattr->attalign)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("table row type and query-specified row type do not match"),
						 errdetail("Physical storage mismatch on dropped attribute at ordinal position %d.",
								   i + 1)));
		}
	}

	/*
	 * Build a composite datum, making sure any toasted fields get detoasted.
	 *
	 * (Note: it is critical that we not change the slot's state here.)
	 */
	tuple = toast_build_flattened_tuple(slot->tts_tupleDescriptor,
										slot->tts_values);
	dtuple = tuple->t_data;

	/*
	 * Label the datum with the composite type info we identified before.
	 *
	 * (Note: we could skip doing this by passing op->d.wholerow.tupdesc to
	 * the tuple build step; but that seems a tad risky so let's not.)
	 */
	HeapTupleHeaderSetTypeId(dtuple, op->d.wholerow.tupdesc->tdtypeid);
	HeapTupleHeaderSetTypMod(dtuple, op->d.wholerow.tupdesc->tdtypmod);

	opres->value = PointerGetDatum(dtuple);
	opres->isnull = false;
}

void
ExecEvalSysVar(ExprState *state, int attnum,
			   TupleTableSlot *slot, NullableDatum *opres)
{
	Datum		d;

	/* slot_getsysattr has sufficient defenses against bad attnums */
	d = slot_getsysattr(slot, attnum, &opres->isnull);
	opres->value = d;
	/* this ought to be unreachable, but it's cheap enough to check */
	if (unlikely(opres->isnull))
		elog(ERROR, "failed to fetch attribute from slot");
}

/*
 * Transition value has not been initialized. This is the first non-NULL input
 * value for a group. We use it as the initial value for transValue.
 */
void
ExecAggInitGroup(const AggStatePerCallContext *percall, AggStatePerGroup pergroup,
				 FunctionCallInfo fcinfo)
{
	MemoryContext oldContext;
	AggStatePerTrans pertrans = percall->pertrans;

	/*
	 * We must copy the datum into aggcontext if it is pass-by-ref. We do not
	 * need to pfree the old transValue, since it's NULL.  (We already checked
	 * that the agg's input type is binary-compatible with its transtype, so
	 * straight copy here is OK.)
	 */
	oldContext = MemoryContextSwitchTo(percall->aggcontext->ecxt_per_tuple_memory);
	pergroup->transValue.value =
		datumCopy(fcinfo->args[1].value,
				  pertrans->transtypeByVal,
				  pertrans->transtypeLen);
	pergroup->transValue.isnull = false;
	pergroup->noTransValue = false;
	MemoryContextSwitchTo(oldContext);
}

/*
 * Ensure that the new transition value is stored in the aggcontext,
 * rather than the per-tuple context.  This should be invoked only when
 * we know (a) the transition data type is pass-by-reference, and (b)
 * the newValue is distinct from the oldValue.
 *
 * NB: This can change the current memory context.
 *
 * We copy the presented newValue into the aggcontext, except when the datum
 * points to a R/W expanded object that is already a child of the aggcontext,
 * in which case we need not copy.  We then delete the oldValue, if not null.
 *
 * If the presented datum points to a R/W expanded object that is a child of
 * some other context, ideally we would just reparent it under the aggcontext.
 * Unfortunately, that doesn't work easily, and it wouldn't help anyway for
 * aggregate-aware transfns.  We expect that a transfn that deals in expanded
 * objects and is aware of the memory management conventions for aggregate
 * transition values will (1) on first call, return a R/W expanded object that
 * is already in the right context, allowing us to do nothing here, and (2) on
 * subsequent calls, modify and return that same object, so that control
 * doesn't even reach here.  However, if we have a generic transfn that
 * returns a new R/W expanded object (probably in the per-tuple context),
 * reparenting that result would cause problems.  We'd pass that R/W object to
 * the next invocation of the transfn, and then it would be at liberty to
 * change or delete that object, and if it deletes it then our own attempt to
 * delete the now-old transvalue afterwards would be a double free.  We avoid
 * this problem by forcing the stored transvalue to always be a flat
 * non-expanded object unless the transfn is visibly doing aggregate-aware
 * memory management.  This is somewhat inefficient, but the best answer to
 * that is to write a smarter transfn.
 */
Datum
ExecAggCopyTransValue(const AggStatePerCallContext *percall,
					  Datum newValue, bool newValueIsNull,
					  NullableDatum *oldValue)
{
	AggStatePerTrans pertrans = percall->pertrans;

	Assert(newValue != oldValue->value);

	if (!newValueIsNull)
	{
		MemoryContextSwitchTo(percall->aggcontext->ecxt_per_tuple_memory);
		if (DatumIsReadWriteExpandedObject(newValue,
										   false,
										   pertrans->transtypeLen) &&
			MemoryContextGetParent(DatumGetEOHP(newValue)->eoh_context) == CurrentMemoryContext)
			 /* do nothing */ ;
		else
			newValue = datumCopy(newValue,
								 pertrans->transtypeByVal,
								 pertrans->transtypeLen);
	}
	else
	{
		/*
		 * Ensure that AggStatePerGroup->transValue ends up being 0, so
		 * callers can safely compare newValue/oldValue without having to
		 * check their respective nullness.
		 */
		newValue = (Datum) 0;
	}

	if (!oldValue->isnull)
	{
		if (DatumIsReadWriteExpandedObject(oldValue->value,
										   false,
										   pertrans->transtypeLen))
			DeleteExpandedObject(oldValue->value);
		else
			pfree(DatumGetPointer(oldValue->value));
	}

	return newValue;
}

/*
 * ExecEvalPreOrderedDistinctSingle
 *		Returns true when the aggregate transition value Datum is distinct
 *		from the previous input Datum and returns false when the input Datum
 *		matches the previous input Datum.
 */
bool
ExecEvalPreOrderedDistinctSingle(const AggStatePerCallContext *percall,
								 AggState *aggstate, AggStatePerTrans pertrans, FunctionCallInfo fcinfo)
{
	Datum		value = fcinfo->args[1].value;
	bool		isnull = fcinfo->args[1].isnull;

	if (!pertrans->haslast ||
		pertrans->lastisnull != isnull ||
		(!isnull && !DatumGetBool(FunctionCall2Coll(&pertrans->equalfnOne,
													pertrans->aggCollation,
													pertrans->lastdatum, value))))
	{
		if (pertrans->haslast && !pertrans->inputtypeByVal &&
			!pertrans->lastisnull)
			pfree(DatumGetPointer(pertrans->lastdatum));

		pertrans->haslast = true;
		if (!isnull)
		{
			MemoryContext oldContext;

			oldContext = MemoryContextSwitchTo(percall->aggcontext->ecxt_per_tuple_memory);

			pertrans->lastdatum = datumCopy(value, pertrans->inputtypeByVal,
											pertrans->inputtypeLen);

			MemoryContextSwitchTo(oldContext);
		}
		else
			pertrans->lastdatum = (Datum) 0;
		pertrans->lastisnull = isnull;
		return true;
	}

	return false;
}

/*
 * ExecEvalPreOrderedDistinctMulti
 *		Returns true when the aggregate input is distinct from the previous
 *		input and returns false when the input matches the previous input.
 */
bool
ExecEvalPreOrderedDistinctMulti(const AggStatePerCallContext *percall, AggState *aggstate, AggStatePerTrans pertrans, FunctionCallInfo fcinfo)
{
	ExprContext *tmpcontext = aggstate->tmpcontext;

	for (int i = 0; i < pertrans->numTransInputs; i++)
	{
		pertrans->sortslot->tts_values[i].value = fcinfo->args[i + 1].value;
		pertrans->sortslot->tts_values[i].isnull = fcinfo->args[i + 1].isnull;
	}

	ExecClearTuple(pertrans->sortslot);
	pertrans->sortslot->tts_nvalid = pertrans->numInputs;
	ExecStoreVirtualTuple(pertrans->sortslot);

	tmpcontext->ecxt_outertuple = pertrans->sortslot;
	tmpcontext->ecxt_innertuple = pertrans->uniqslot;

	if (!pertrans->haslast ||
		!ExecQual(pertrans->equalfnMulti, tmpcontext))
	{
		if (pertrans->haslast)
			ExecClearTuple(pertrans->uniqslot);

		pertrans->haslast = true;
		ExecCopySlot(pertrans->uniqslot, pertrans->sortslot);
		return true;
	}
	return false;
}

/*
 * Invoke ordered transition function, with a datum argument.
 */
void
ExecEvalAggOrderedTransDatum(ExprState *state, const ExprEvalStep *op,
							 NullableDatum *columns)
{
	AggStatePerTrans pertrans = op->d.agg_trans_ordered.pertrans;
	int			setno = op->d.agg_trans_ordered.setno;

	tuplesort_putdatum(pertrans->sortstates[setno],
					   columns[0].value, columns[0].isnull);
}

/*
 * Invoke ordered transition function, with a tuple argument.
 */
void
ExecEvalAggOrderedTransTuple(ExprState *state, const ExprEvalStep *op,
							 NullableDatum *columns)
{
	AggStatePerTrans pertrans = op->d.agg_trans_ordered.pertrans;
	int			setno = op->d.agg_trans_ordered.setno;

	ExecClearTuple(pertrans->sortslot);

	memcpy(pertrans->sortslot->tts_values, columns, sizeof(NullableDatum) * pertrans->numInputs);
	pertrans->sortslot->tts_nvalid = pertrans->numInputs;
	ExecStoreVirtualTuple(pertrans->sortslot);

	tuplesort_puttupleslot(pertrans->sortstates[setno], pertrans->sortslot);
}

/* implementation of transition function invocation for byval types */
static pg_attribute_always_inline void
ExecAggPlainTransByVal(AggState *aggstate, const AggStatePerCallContext *percall,
					   AggStatePerGroup pergroup,
					   FunctionCallInfo fcinfo, PGFunction fn_addr)
{
	MemoryContext oldContext;
	Datum		newVal;

	/* invoke transition function in per-tuple context */
	oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);

	fcinfo->context = (void *) percall;
	fcinfo->args[0] = pergroup->transValue;
	fcinfo->isnull = false;		/* just in case transfn doesn't set it */

	newVal = fn_addr(fcinfo);

	pergroup->transValue.value = newVal;
	pergroup->transValue.isnull = fcinfo->isnull;

	MemoryContextSwitchTo(oldContext);
}

/* implementation of transition function invocation for byref types */
static pg_attribute_always_inline void
ExecAggPlainTransByRef(AggState *aggstate, const AggStatePerCallContext *percall,
					   AggStatePerGroup pergroup,
					   FunctionCallInfo fcinfo, PGFunction fn_addr)
{
	MemoryContext oldContext;
	Datum		newVal;

	/* invoke transition function in per-tuple context */
	oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);

	fcinfo->context = (void *) percall;
	fcinfo->args[0] = pergroup->transValue;
	fcinfo->isnull = false;		/* just in case transfn doesn't set it */

	newVal = fn_addr(fcinfo);

	/*
	 * For pass-by-ref datatype, must copy the new value into aggcontext and
	 * free the prior transValue.  But if transfn returned a pointer to its
	 * first input, we don't need to do anything.
	 *
	 * It's safe to compare newVal with pergroup->transValue without regard
	 * for either being NULL, because ExecAggCopyTransValue takes care to set
	 * transValue to 0 when NULL. Otherwise we could end up accidentally not
	 * reparenting, when the transValue has the same numerical value as
	 * newValue, despite being NULL.  This is a somewhat hot path, making it
	 * undesirable to instead solve this with another branch for the common
	 * case of the transition function returning its (modified) input
	 * argument.
	 */
	if (DatumGetPointer(newVal) != DatumGetPointer(pergroup->transValue.value))
		newVal = ExecAggCopyTransValue(percall,
									   newVal, fcinfo->isnull,
									   &pergroup->transValue);

	pergroup->transValue.value = newVal;
	pergroup->transValue.isnull = fcinfo->isnull;

	MemoryContextSwitchTo(oldContext);
}
