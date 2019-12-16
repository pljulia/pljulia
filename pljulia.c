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

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pljulia_call_handler);

/*
 * Handle function, procedure, and trigger calls.
 */
Datum
pljulia_call_handler(PG_FUNCTION_ARGS)
{
	return 0;
}
