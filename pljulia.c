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

#include <sys/time.h>
#include <julia.h>
#include "convert_args.h"

#define DOUBLE_LEN 316
#define LONG_INT_LEN 20

/**********************************************************************
 * The information we cache about loaded procedures.
 **********************************************************************/
typedef struct pljulia_proc_desc
{
	/* the name given by the user upon function definition */
	char *user_proname;
	char *internal_proname; /* Julia name (based on function OID) */
	/* context holding this procedure and its subsidiaries */
	MemoryContext mcxt;
	Oid result_typid; /* OID of fn's result type */
	int nargs; /* number of arguments */
	TransactionId fn_xmin;
	char *function_body;
} pljulia_proc_desc;

/* The procedure hash key */
typedef struct pljulia_proc_key
{
	/*
	 * just one field for now but define a struct as it will be useful later
	 * on to add more fields
	 */
	Oid fn_oid;
} pljulia_proc_key;

/* The procedure hash entry */
typedef struct pljulia_hash_entry
{
	pljulia_proc_key proc_key;
	pljulia_proc_desc *prodesc;
} pljulia_hash_entry;

/* The hash table we use to lookup the function in case it already exists */
static HTAB *pljulia_proc_hashtable = NULL;

MemoryContext TopMemoryContext = NULL;

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pljulia_call_handler);

static Datum cstring_to_type(char *, Oid);
static Datum jl_value_t_to_datum(FunctionCallInfo, jl_value_t *, Oid);
pljulia_proc_desc *pljulia_compile(FunctionCallInfo, HeapTuple, Form_pg_proc);
static Datum pljulia_execute(FunctionCallInfo);
void julia_setup_input_args(FunctionCallInfo, HeapTuple, Form_pg_proc,
		jl_value_t **, MemoryContext);
void _PG_init(void);

void
_PG_init(void)
{
	double jl_init_time;
	struct timeval t1, t2;

	gettimeofday(&t1, NULL);
	/* required: setup the Julia context */
	jl_init();
	gettimeofday(&t2, NULL);
	jl_init_time = (double) (t2.tv_usec - t1.tv_usec) / 1000 +
			(double) (t2.tv_sec - t1.tv_sec) * 1000;
	elog(DEBUG1, "Julia initialized in %f milliseconds.", jl_init_time);

	/*
	 * Initialize the hash table
	 */
	HASHCTL hash_ctl;

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(pljulia_proc_key);
	hash_ctl.entrysize = sizeof(pljulia_hash_entry);

	pljulia_proc_hashtable =
			hash_create("PL/Julia cached procedures hashtable", 32, &hash_ctl,
					HASH_ELEM);
}

/*
 * Convert the C string "input" to a Datum of type "typeoid".
 */
static Datum
cstring_to_type(char *input, Oid typeoid)
{
	Oid typInput, typIOParam;
	Datum ret;

	getTypeInputInfo(typeoid, &typInput, &typIOParam);
	ret = OidFunctionCall3(typInput, CStringGetDatum(input), 0, -1);

	PG_RETURN_DATUM(ret);
}

/*
 * Convert the Julia result to a Datum of type "typeoid".
 */
static Datum
jl_value_t_to_datum(FunctionCallInfo fcinfo, jl_value_t *ret, Oid prorettype)
{
	char *buffer;

	if (jl_is_string(ret))
	{
		elog(DEBUG1, "ret (string): %s", jl_string_ptr(ret));
		PG_RETURN_DATUM(cstring_to_type((char *) jl_string_ptr(ret),
					prorettype));
	}
	else if (jl_typeis(ret, jl_float64_type))
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
	else if (jl_typeis(ret, jl_bool_type))
	{
		int ret_unboxed  = jl_unbox_bool(ret);
		elog(DEBUG1, "ret (bool): %d", jl_unbox_bool(ret));

		buffer = (char *) palloc0((LONG_INT_LEN + 1) * sizeof(char));
		snprintf(buffer, LONG_INT_LEN, "%d", ret_unboxed);
	}
	else
	{
		elog(ERROR, "ERROR: unexpected unboxed Julia return type");
		PG_RETURN_NULL();
	}
	elog(DEBUG1, "ret (buffer): %s", buffer);

	PG_RETURN_DATUM(cstring_to_type(buffer, prorettype));
}

void
julia_setup_input_args(FunctionCallInfo fcinfo, HeapTuple procedure_tuple,
		Form_pg_proc procedure_struct, jl_value_t **boxed_args,
		MemoryContext proc_cxt)
{
	Oid *argtypes;
	char **argnames;
	char *argmodes;
	char *value;
	int i;
	FmgrInfo *arg_out_func;
	Form_pg_type type_struct;
	HeapTuple type_tuple;

	get_func_arg_info(procedure_tuple, &argtypes, &argnames, &argmodes);
	arg_out_func = (FmgrInfo *) palloc0(fcinfo->nargs * sizeof(FmgrInfo));

	for (i = 0; i < fcinfo->nargs; i++)
	{
		Oid argtype = procedure_struct->proargtypes.values[i];

		type_tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(argtype));
		if (!HeapTupleIsValid(type_tuple))
			elog(ERROR, "cache lookup failed for type %u", argtype);

		type_struct = (Form_pg_type) GETSTRUCT(type_tuple);
		fmgr_info_cxt(type_struct->typoutput, &(arg_out_func[i]), proc_cxt);
		ReleaseSysCache(type_tuple);

		value = OutputFunctionCall(&arg_out_func[i], fcinfo->args[i].value);
		boxed_args[i] = pg_oid_to_julia(argtypes[i], value);

		elog(DEBUG1, "[%d] %s = %s :: %u", i + 1, argnames[i], value, argtypes[i]);
	}
}

/*
 * Handle function, procedure, and trigger calls.
 */
Datum pljulia_call_handler(PG_FUNCTION_ARGS)
{
	Datum ret;

	/* run Julia code */
	ret = pljulia_execute(fcinfo);

	/*
	 * strongly recommended: notify Julia that the program is about to
	 * terminate. this allows Julia time to cleanup pending write requests and
	 * run all finalizers
	 */
	jl_atexit_hook(0);

	return ret;
}

/*
 * Retrieve Julia code and create a user-defined function
 * with a unique name.
 */
pljulia_proc_desc *
pljulia_compile(FunctionCallInfo fcinfo, HeapTuple procedure_tuple,
		Form_pg_proc procedure_struct)
{
	Datum procedure_source_datum;
	const char *procedure_code;
	bool isnull;
	volatile MemoryContext proc_cxt = NULL;
	MemoryContext oldcontext;

	int compiled_len = 0;
	char *compiled_code;
	pljulia_proc_desc *prodesc = NULL;

	int i;
	FmgrInfo *arg_out_func;
	Form_pg_type type_struct;
	HeapTuple type_tuple;

	Oid *argtypes;
	char **argnames;
	char *argmodes;
	char *value;

	bool found_hashentry;
	pljulia_proc_key proc_key;
	pljulia_hash_entry *hash_entry;

	/* First try to find the function in the lookup table */
	proc_key.fn_oid = fcinfo->flinfo->fn_oid;
	hash_entry = hash_search(pljulia_proc_hashtable, &proc_key, HASH_FIND,
			&found_hashentry);

	if (found_hashentry)
		prodesc = hash_entry->prodesc;

	if (prodesc != NULL)
	{
		/* Check that it hasn't been modified (CREATE OR REPLACE) */
		if (prodesc->fn_xmin ==
				HeapTupleHeaderGetRawXmin(procedure_tuple->t_data))
		{
			/* it's ok to return it, hasn't been modified */
			return prodesc;
		}
		else
		{
			/* remove this outdated entry from the hash table */
			/*
			 * found_hashentry is redundant since we don't expect a returned entry from
			 * HASH_REMOVE
			 */
			hash_search(pljulia_proc_hashtable, &proc_key, HASH_REMOVE, NULL);
		}
	}

	/***************************************************
	 * At this point prodesc is either NULL or outdated
	 * so we have to create a new entry for the function
	 ***************************************************/

	/*
	 * the length here is arbitrary but should be enough for at-most-32-length
	 * oid and even the NAMEDATALEN-length function name should we decide to
	 * include that too
	 */
	char internal_procname[256];

	procedure_source_datum = SysCacheGetAttr(PROCOID, procedure_tuple,
			Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc");

	procedure_code = DatumGetCString(DirectFunctionCall1(textout,
				procedure_source_datum));
	elog(DEBUG1, "procedure code:\n%s", procedure_code);

	/*
	 * Add the final carriage return to the length of the original procedure.
	 */
	compiled_len += strlen(procedure_code) + 1;

	arg_out_func = (FmgrInfo *) palloc0(fcinfo->nargs * sizeof(FmgrInfo));

	proc_cxt = AllocSetContextCreate(TopMemoryContext, "PL/Julia function",
			ALLOCSET_SMALL_SIZES);

	get_func_arg_info(procedure_tuple, &argtypes, &argnames, &argmodes);

	elog(DEBUG1, "nargs %d", fcinfo->nargs);

	snprintf(internal_procname, sizeof(internal_procname), "pljulia_%u",
			fcinfo->flinfo->fn_oid);
	/* +1 is for the line break (\n) */
	compiled_len += strlen("function ") + 1;
	i = strlen(internal_procname);
	compiled_len += i;
	internal_procname[i] = '\0';
	/* one \n and '(' and ')' */
	compiled_len += strlen("end") + 3;

	/*
	 * Loop through the parameters to determine how big of a buffer is needed
	 * for prepending the parameter names as input arguments to the
	 * function/procedure code.
	 */
	for (i = 0; i < fcinfo->nargs; i++)
	{
		Oid argtype = procedure_struct->proargtypes.values[i];

		type_tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(argtype));
		if (!HeapTupleIsValid(type_tuple))
			elog(ERROR, "cache lookup failed for type %u", argtype);

		type_struct = (Form_pg_type) GETSTRUCT(type_tuple);
		fmgr_info_cxt(type_struct->typoutput, &(arg_out_func[i]), proc_cxt);
		ReleaseSysCache(type_tuple);

		value = OutputFunctionCall(&arg_out_func[i], fcinfo->args[i].value);

		elog(DEBUG1, "[%d] %s = %s :: %u", i, argnames[i], value, argtypes[i]);

		/* Factor in length of a ',' */
		compiled_len += strlen(argnames[i]) + 1;
	}
	elog(DEBUG1, "compiled_len = %d", compiled_len);

	oldcontext = MemoryContextSwitchTo(proc_cxt);
	/* stuff that the new prodesc uses must be palloc'd in this context */
	compiled_code = (char *) palloc0(compiled_len * sizeof(char));

	/*
	 * Declare the procedure code as a function with the input parameters as
	 * the function arguments.
	 */
	compiled_code[0] = '\0';
	strcpy(compiled_code, "function ");
	strcat(compiled_code, internal_procname);
	/* now the argument names */
	strcat(compiled_code, "(");
	for (i = 0; i < fcinfo->nargs; i++)
	{
		strcat(compiled_code, argnames[i]);
		strcat(compiled_code, ",");
	}
	/* remove the last ',' if present, close ')' */
	if (i > 0)
	{
		i = strlen(compiled_code) - 1;
		compiled_code[i] = ')';
	}
	else
		strcat(compiled_code, ")");
	strcat(compiled_code, procedure_code);
	strcat(compiled_code, "\nend");
	elog(DEBUG1, "compiled code (%ld)\n%s", strlen(compiled_code),
			compiled_code);

	prodesc = (pljulia_proc_desc *) palloc0(sizeof(pljulia_proc_desc));
	if (!prodesc)
		elog(ERROR, "pljulia: out of memory");
	prodesc->function_body = compiled_code;
	prodesc->user_proname = pstrdup(NameStr(procedure_struct->proname));
	prodesc->internal_proname = pstrdup(internal_procname);
	prodesc->nargs = fcinfo->nargs;
	prodesc->result_typid = procedure_struct->prorettype;
	prodesc->mcxt = proc_cxt;
	prodesc->fn_xmin = HeapTupleHeaderGetRawXmin(procedure_tuple->t_data);
	MemoryContextSwitchTo(oldcontext);

	/* Create a new hashtable entry for the new function definition */
	hash_entry = hash_search(pljulia_proc_hashtable, &proc_key, HASH_ENTER,
			&found_hashentry);
	if (hash_entry == NULL)
		elog(ERROR, "pljulia: hash table out of memory");
	hash_entry->prodesc = prodesc;

	/* insert function declaration into Julia */
	jl_eval_string(compiled_code);
	if (jl_exception_occurred())
		elog(ERROR, "%s", jl_typeof_str(jl_exception_occurred()));

	return prodesc;
}

/*
 * Execute Julia code and handle the data returned by Julia.
 */
static Datum
pljulia_execute(FunctionCallInfo fcinfo)
{
	jl_value_t **boxed_args;
	HeapTuple procedure_tuple;
	Form_pg_proc procedure_struct;
	int i;
	jl_value_t *ret;

	pljulia_proc_desc *prodesc = NULL;

	procedure_tuple = SearchSysCache(PROCOID,
			ObjectIdGetDatum(fcinfo->flinfo->fn_oid), 0, 0, 0);
	if (!HeapTupleIsValid(procedure_tuple))
		elog(ERROR, "cache lookup failed for function %u",
				fcinfo->flinfo->fn_oid);
	procedure_struct = (Form_pg_proc) GETSTRUCT(procedure_tuple);

	/* function definition + body code */
	prodesc = pljulia_compile(fcinfo, procedure_tuple, procedure_struct);

	ReleaseSysCache(procedure_tuple);

	/*
	 * this assumes that pljulia_compile inserts the function definition into
	 * the interpreter
	 */
	jl_function_t *func = jl_get_function(jl_main_module,
			prodesc->internal_proname);

	if (jl_exception_occurred())
		elog(ERROR, "%s", jl_typeof_str(jl_exception_occurred()));

	/*
	 * insert the function code into the julia interpreter then get a pointer
	 * to the function and call it after the arguments are settled too .
	 * Allocate the space for the arguments here
	 */
	boxed_args = (jl_value_t **) palloc0(prodesc->nargs * sizeof(jl_value_t *));

	julia_setup_input_args(fcinfo, procedure_tuple, procedure_struct,
			boxed_args, prodesc->mcxt);
	if (jl_exception_occurred())
		elog(ERROR, "%s", jl_typeof_str(jl_exception_occurred()));
	ret = jl_call(func, boxed_args, prodesc->nargs);
	if (jl_exception_occurred())
		elog(ERROR, "%s", jl_typeof_str(jl_exception_occurred()));

	return jl_value_t_to_datum(fcinfo, ret, procedure_struct->prorettype);
}
