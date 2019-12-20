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

#include <julia.h>

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pljulia_call_handler);

/*
 * Handle function, procedure, and trigger calls.
 */
Datum
pljulia_call_handler(PG_FUNCTION_ARGS)
{
	jl_value_t *ret;
	double ret_unboxed;

	/* required: setup the Julia context */
	jl_init();

	/* run Julia command */
	ret = jl_eval_string("sqrt(2.0)");
	if (jl_typeis(ret, jl_float64_type))
	{
		ret_unboxed = jl_unbox_float64(ret);
		elog(INFO, "sqrt(2.0) in C: %e", ret_unboxed);
	}
	else
		elog(INFO, "ERROR: unexpected return type from sqrt(::Float64)");

	/* strongly recommended: notify Julia that the
	 * program is about to terminate. this allows Julia time to cleanup
	 * pending write requests and run all finalizers
	 */
	jl_atexit_hook(0);

	return 0;
}
