/*-------------------------------------------------------------------------
 *
 * pljulia.c - Handler for the PL/Julia
 *             procedural language
 *
 * Portions Copyright (c) 2019, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>
#include <fmgr.h>
#include <funcapi.h>
#include <access/htup_details.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_type.h>
#include <utils/memutils.h>
#include <utils/builtins.h>
#include <utils/syscache.h>

#include <julia.h>

MemoryContext TopMemoryContext = NULL;

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pljulia_call_handler);

void pljulia_execute(FunctionCallInfo);

/*
 * Handle function, procedure, and trigger calls.
 */
Datum
pljulia_call_handler(PG_FUNCTION_ARGS)
{
	/* required: setup the Julia context */
	jl_init();

	/* run Julia code */
	pljulia_execute(fcinfo);

	/* strongly recommended: notify Julia that the
	 * program is about to terminate. this allows Julia time to cleanup
	 * pending write requests and run all finalizers
	 */
	jl_atexit_hook(0);

	return 0;
}

/*
 * Retrieve the Julia code and execute it.
 */
void
pljulia_execute(FunctionCallInfo fcinfo)
{
	HeapTuple procedure_tuple;
	Form_pg_proc procedure_struct;
	Datum procedure_source_datum;
	const char *procedure_code;
	bool isnull;

	jl_value_t *ret;

	procedure_tuple = SearchSysCache(PROCOID,
			ObjectIdGetDatum(fcinfo->flinfo->fn_oid), 0, 0, 0);
	if (!HeapTupleIsValid(procedure_tuple))
		elog(ERROR, "cache lookup failed for function %u",
				fcinfo->flinfo->fn_oid);
	procedure_struct = (Form_pg_proc) GETSTRUCT(procedure_tuple);

	procedure_source_datum = SysCacheGetAttr(PROCOID, procedure_tuple,
			Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc");

	procedure_code = DatumGetCString(DirectFunctionCall1(textout,
			procedure_source_datum));
	elog(DEBUG1, "procedure code:\n%s", procedure_code);

	ret = jl_eval_string(procedure_code);
	if (jl_exception_occurred())
		elog(ERROR, "%s", jl_typeof_str(jl_exception_occurred()));

	if (jl_typeis(ret, jl_float64_type))
	{
		double ret_unboxed = jl_unbox_float64(ret);
		elog(DEBUG1, "ret: %e", ret_unboxed);
	}
	else
	{
		elog(ERROR, "ERROR: unexpected return type");
	}
}
