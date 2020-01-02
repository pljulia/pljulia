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

#define DOUBLE_LEN 316
#define LONG_INT_LEN 20

MemoryContext TopMemoryContext = NULL;

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pljulia_call_handler);

static Datum cstring_to_type(char *, Oid);
char *pljulia_compile(FunctionCallInfo, HeapTuple, Form_pg_proc);
static Datum pljulia_execute(FunctionCallInfo);

/*
 * Convert the C string "input" to a Datum of type "typeoid".
 */
static Datum
cstring_to_type(char * input, Oid typeoid)
{
	HeapTuple typetuple;
	Form_pg_type pg_type_entry;
	Datum ret;

	typetuple = SearchSysCache(TYPEOID, ObjectIdGetDatum(typeoid), 0, 0, 0);
	if (!HeapTupleIsValid(typetuple))
		elog(ERROR, "cache lookup failed for type %u", typeoid);

	pg_type_entry = (Form_pg_type) GETSTRUCT(typetuple);

	ret = OidFunctionCall3(pg_type_entry->typinput,
						   CStringGetDatum(input),
						   0, -1);

	ReleaseSysCache(typetuple);

	PG_RETURN_DATUM(ret);
}

/*
 * Handle function, procedure, and trigger calls.
 */
Datum
pljulia_call_handler(PG_FUNCTION_ARGS)
{
	Datum ret;

	/* required: setup the Julia context */
	jl_init();

	/* run Julia code */
	ret = pljulia_execute(fcinfo);

	/* strongly recommended: notify Julia that the
	 * program is about to terminate. this allows Julia time to cleanup
	 * pending write requests and run all finalizers
	 */
	jl_atexit_hook(0);

	return ret;
}

/*
 * Retrieve Julia code and reconstruct it with parameter inputs as Julia global
 * variables.
 */
char *
pljulia_compile(FunctionCallInfo fcinfo, HeapTuple procedure_tuple,
		Form_pg_proc procedure_struct)
{
	Datum procedure_source_datum;
	const char *procedure_code;
	bool isnull;
	volatile MemoryContext proc_cxt = NULL;

	int compiled_len = 0;
	char *compiled_code;

	int i;
	FmgrInfo *arg_out_func;
	Form_pg_type type_struct;
	HeapTuple type_tuple;

	Oid *argtypes;
	char **argnames;
	char *argmodes;
	char *value;

	procedure_source_datum = SysCacheGetAttr(PROCOID, procedure_tuple,
			Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc");

	procedure_code = DatumGetCString(DirectFunctionCall1(textout,
			procedure_source_datum));
	elog(DEBUG1, "procedure code:\n%s", procedure_code);

	/*
	 * Add the final carriage return to the length of the original
	 * procedure.
	 */
	compiled_len += strlen(procedure_code) + 1;

	arg_out_func = (FmgrInfo *) palloc0(fcinfo->nargs * sizeof(FmgrInfo));

	proc_cxt = AllocSetContextCreate(TopMemoryContext,
			"PL/Julia function", 0, (1 * 1024), (8 * 1024));

	get_func_arg_info(procedure_tuple, &argtypes, &argnames, &argmodes);

	elog(DEBUG1, "nargs %d", fcinfo->nargs);

	/*
	 * Loop through the parameters to determine how big of a buffer is needed
	 * for prepending the parameters as global variables to the
	 * function/procedure code.
	 */
	for (i = 0; i < fcinfo->nargs; i ++)
	{
		Oid argtype = procedure_struct->proargtypes.values[i];
		type_tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(argtype));
		if (!HeapTupleIsValid(type_tuple))
			elog(ERROR, "cache lookup failed for type %u", argtype);

		type_struct = (Form_pg_type) GETSTRUCT(type_tuple);
		fmgr_info_cxt(type_struct->typoutput, &(arg_out_func[i]), proc_cxt);

		value = OutputFunctionCall(&arg_out_func[i], fcinfo->args[i].value);

		elog(DEBUG1, "[%d] %s = %s", i, argnames[i], value);

		/* Factor in length of an equal sign (=) and a line break (\n). */
		compiled_len += strlen(argnames[i]) + strlen(value) + 2;
	}
	elog(DEBUG1, "compiled_len = %d", compiled_len);

	compiled_code = (char *) palloc0(compiled_len * sizeof(char));

	/*
	 * Prepend the procedure code with the input parameters as global
	 * variables.
	 */
	compiled_code[0] = '\0';
	for (i = 0; i < fcinfo->nargs; i ++)
	{
		strcat(compiled_code, argnames[i]);
		strcat(compiled_code, "=");
		value = OutputFunctionCall(&arg_out_func[i], fcinfo->args[i].value);
		strcat(compiled_code, value);
		strcat(compiled_code, "\n");
	}
	strcat(compiled_code, procedure_code);
	elog(DEBUG1, "compiled code (%ld)\n%s", strlen(compiled_code),
			compiled_code);

	return compiled_code;
}

/*
 * Execute Julia code and handle the data returned by Julia.
 */
static Datum
pljulia_execute(FunctionCallInfo fcinfo)
{
	char *buffer;
	jl_value_t *ret;

	HeapTuple procedure_tuple;
	Form_pg_proc procedure_struct;

	char *code;

	procedure_tuple = SearchSysCache(PROCOID,
			ObjectIdGetDatum(fcinfo->flinfo->fn_oid), 0, 0, 0);
	if (!HeapTupleIsValid(procedure_tuple))
		elog(ERROR, "cache lookup failed for function %u",
				fcinfo->flinfo->fn_oid);
	procedure_struct = (Form_pg_proc) GETSTRUCT(procedure_tuple);

	code = pljulia_compile(fcinfo, procedure_tuple, procedure_struct);

	ret = jl_eval_string(code);
	if (jl_exception_occurred())
		elog(ERROR, "%s", jl_typeof_str(jl_exception_occurred()));

	if (jl_typeis(ret, jl_float64_type))
	{
		double ret_unboxed = jl_unbox_float64(ret);
		elog(DEBUG1, "ret (float64): %f", jl_unbox_float64(ret));

		buffer = (char *) palloc0((DOUBLE_LEN + 1) * sizeof(char));
		snprintf(buffer, DOUBLE_LEN, "%f", ret_unboxed);
	}
	else if (jl_typeis(ret, jl_int64_type))
	{
		long int ret_unboxed = jl_unbox_int64(ret);
		elog(DEBUG1, "ret (int64): %ld", jl_unbox_int64(ret));

		buffer = (char *) palloc0((LONG_INT_LEN + 1) * sizeof(char));
		snprintf(buffer, LONG_INT_LEN, "%ld", ret_unboxed);
	}
	else
	{
		elog(ERROR, "ERROR: unexpected unboxed Julia return type");
		PG_RETURN_NULL();
	}
	elog(DEBUG1, "ret (buffer): %s", buffer);

	PG_RETURN_DATUM(cstring_to_type(buffer, procedure_struct->prorettype));
}
