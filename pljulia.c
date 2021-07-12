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
#include <utils/typcache.h>
#include <utils/rel.h>
#include <utils/fmgroids.h>
#include <funcapi.h>
#include <miscadmin.h>
#include <executor/spi.h>
#include <commands/trigger.h>
#include "mb/pg_wchar.h"
#include <commands/event_trigger.h>
#include <utils/guc.h>

#include <sys/time.h>
#include <julia.h>
#include "convert_args.h"

#define DOUBLE_LEN 316
#define LONG_INT_LEN 20
#define jl_is_dict(ret) (strcmp(jl_typeof_str(ret), "Dict") == 0)
#define jl_is_bigfloat(ret) (strcmp(jl_typeof_str(ret), "BigFloat") == 0)

#define show_julia_error()             \
	elog(ERROR, "%s",                  \
	     jl_string_ptr(jl_eval_string( \
	         "sprint(showerror, ccall(:jl_exception_occurred, Any, ()))")))

/**********************************************************************
 * The information we cache about loaded procedures.
 **********************************************************************/
typedef struct pljulia_proc_desc
{
	/* the name given by the user upon function definition */
	char	   *user_proname;
	char	   *internal_proname;	/* Julia name (based on function OID) */

	/*
	 * context holding this procedure and its subsidiaries analogous to
	 * plpython
	 */
	MemoryContext mcxt;
	Oid			result_typid;	/* OID of fn's result type */
	int			nargs;			/* number of arguments */
	TransactionId fn_xmin;
	char	   *function_body;
	FmgrInfo   *arg_out_func;	/* output fns for arg types, kept to convert
								 * from datum to cstring */
	Oid		   *arg_arraytype;	/* InvalidOid if not an array */
	bool	   *arg_is_rowtype; /* is the argument composite? */
	bool		fn_retisset;	/* true if function returns set (SRF) */
	bool		fn_retistuple;	/* true if function returns composite */
} pljulia_proc_desc;

/* The procedure hash key */
typedef struct pljulia_proc_key
{
	/*
	 * is_trigger is in fact a bool, declared as Oid to ensure this struct
	 * contains no padding
	 */
	Oid			fn_oid;
	Oid			is_trigger;
} pljulia_proc_key;

/* The procedure hash entry */
typedef struct pljulia_hash_entry
{
	pljulia_proc_key proc_key;
	pljulia_proc_desc *prodesc;
} pljulia_hash_entry;

/* This struct holds information about a single function call */
typedef struct pljulia_call_data
{
	FunctionCallInfo fcinfo;
	pljulia_proc_desc *prodesc;

	/*
	 * Information for SRFs and functions returning composite types.
	 */
	TupleDesc	ret_tupdesc;	/* return rowtype, if retistuple or retisset */
	AttInMetadata *attinmeta;	/* metadata for building tuples of that type */
	Tuplestorestate *tuple_store;	/* SRFs accumulate result here */
	MemoryContext tmp_cxt;		/* context for tuplestore */
} pljulia_call_data;

/* This is saved and restored by pljulia_call_handler */
static pljulia_call_data *current_call_data = NULL;

/* The hash table we use to lookup the function in case it already exists */
static HTAB *pljulia_proc_hashtable = NULL;

MemoryContext TopMemoryContext = NULL;

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pljulia_call_handler);
PG_FUNCTION_INFO_V1(pljulia_inline_handler);

static Datum cstring_to_type(char *, Oid);
static Datum jl_value_t_to_datum(FunctionCallInfo, jl_value_t *, Oid, bool);
pljulia_proc_desc *pljulia_compile(FunctionCallInfo, HeapTuple, Form_pg_proc, bool, bool);
static Datum pljulia_execute(FunctionCallInfo);
void		julia_setup_input_args(FunctionCallInfo, HeapTuple, Form_pg_proc,
								   jl_value_t **, pljulia_proc_desc *);
jl_value_t *convert_arg_to_julia(Datum, Oid, pljulia_proc_desc *, int);
jl_value_t *julia_array_from_datum(Datum, Oid);
jl_value_t *julia_dict_from_datum(Datum);
jl_value_t *pljulia_dict_from_tuple(HeapTuple, TupleDesc, bool);

Datum		pg_array_from_julia_array(FunctionCallInfo, jl_value_t *, Oid);
Datum		pg_composite_from_julia_tuple(FunctionCallInfo, jl_value_t *, Oid, bool);
Datum		pg_composite_from_julia_dict(FunctionCallInfo, jl_value_t *, Oid, bool);
static Datum pljulia_trigger_handler(FunctionCallInfo);
static void pljulia_event_trigger_handler(FunctionCallInfo);
Datum		pljulia_validator(FunctionCallInfo);

void		_PG_init(void);
static HeapTuple pljulia_build_tuple_result(jl_value_t *, TupleDesc);
void		pljulia_return_next(jl_value_t *);
void		pljulia_elog(jl_value_t *, jl_value_t *);

/* these are taken from pltcl so it would be good to find a way
 * to include them from the source code instead of copying them */
static inline char *
utf_u2e(const char *src)
{
	return pg_any_to_server(src, strlen(src), PG_UTF8);
}

static inline char *
utf_e2u(const char *src)
{
	return pg_server_to_any(src, strlen(src), PG_UTF8);
}

void
pljulia_return_next(jl_value_t *obj)
{
	pljulia_call_data *call_data;
	FunctionCallInfo fcinfo;
	pljulia_proc_desc *prodesc;
	MemoryContext old_cxt;
	ReturnSetInfo *rsi;

	call_data = current_call_data;
	fcinfo = call_data->fcinfo;
	prodesc = call_data->prodesc;
	rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	if (!prodesc->fn_retisset)
		elog(ERROR, "return_next called in function that doesn't return set");

	/* Set up tuple store if first output row */
	if (!call_data->ret_tupdesc)
	{
		TupleDesc	tupdesc;

		if (prodesc->fn_retistuple)
		{
			Oid			typid;

			if (get_call_result_type(fcinfo, &typid, &tupdesc) !=
				TYPEFUNC_COMPOSITE)
				elog(ERROR, "function returning record called in context that "
					 "cannot accept type record");
		}
		else
		{
			tupdesc = rsi->expectedDesc;
			if (tupdesc == NULL || tupdesc->natts != 1)
				elog(ERROR, "expected single-column result descriptor for "
					 "non-composite SETOF result");
		}

		/*
		 * Make sure the tuple_store and ret_tdesc are sufficiently
		 * long-lived.
		 */
		old_cxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);

		current_call_data->ret_tupdesc = CreateTupleDescCopy(tupdesc);
		current_call_data->tuple_store = tuplestore_begin_heap(rsi->allowedModes
															   & SFRM_Materialize_Random,
															   false, work_mem);

		MemoryContextSwitchTo(old_cxt);
	}
	/* done with first-call initializations */
	if (!current_call_data->tmp_cxt)
	{
		current_call_data->tmp_cxt = AllocSetContextCreate(
														   CurrentMemoryContext, "PL/Julia return_next temp context",
														   ALLOCSET_SMALL_SIZES);
	}
	old_cxt = MemoryContextSwitchTo(current_call_data->tmp_cxt);

	if (prodesc->fn_retistuple)
	{
		HeapTuple	tuple;

		tuple = pljulia_build_tuple_result(obj, current_call_data->ret_tupdesc);
		tuplestore_puttuple(call_data->tuple_store, tuple);
	}
	else if (prodesc->result_typid)
	{
		Datum		ret[1];
		bool		isNull[1];

		if (!obj || jl_is_nothing(obj))
			isNull[0] = true;
		else
			isNull[0] = false;
		ret[0] = jl_value_t_to_datum(fcinfo, obj, prodesc->result_typid, false);
		tuplestore_putvalues(call_data->tuple_store, call_data->ret_tupdesc,
							 ret, isNull);
	}
	MemoryContextSwitchTo(old_cxt);
	MemoryContextReset(current_call_data->tmp_cxt);
}

void
pljulia_elog(jl_value_t *lvl, jl_value_t *msg)
{
	volatile int level;
	MemoryContext oldcontext;
	int			priority_idx;
	int			i;

	static const char *logpriorities[] = {
		"DEBUG", "LOG", "INFO", "NOTICE",
		"WARNING", "ERROR", "FATAL", (const char *) NULL
	};

	static const int loglevels[] = {
		DEBUG1, LOG, INFO, NOTICE,
		WARNING, ERROR, FATAL
	};

	for (i = 0; i < 8; i++)
	{
		if (strcmp(logpriorities[i], jl_string_ptr(lvl)) == 0)
		{
			priority_idx = i;
			break;
		}
	}
	if (i >= 7)
		elog(ERROR, "no match found for elog levels");
	else
	{
		level = loglevels[priority_idx];
		elog(level, jl_string_ptr(msg));
	}
	return;
}



/*
 * takes a Julia tuple or dictionary and a TupleDesc as input,
 * and returns a heaptuple to use in a SRF tuplestore.
 */
static HeapTuple
pljulia_build_tuple_result(jl_value_t *obj, TupleDesc tupdesc)
{
	Datum	   *values;
	bool	   *nulls;
	HeapTuple	tup;
	int			nfields;
	Form_pg_attribute att;
	int			i;
	jl_value_t *curr_elem;

	if (jl_is_dict(obj))
	{
		jl_function_t *dict_nfields;

		dict_nfields = jl_get_function(jl_base_module, "length");

		nfields = jl_unbox_int64(jl_call1(dict_nfields, obj));
		if (tupdesc->natts != nfields)
			elog(ERROR, "Dict number of fields mismatch");
	}
	else
	{
		nfields = jl_nfields(obj);
		if (tupdesc->natts != nfields)
			elog(ERROR, "Tuple number of fields mismatch");
	}

	values = (Datum *) palloc0(sizeof(Datum) * nfields);
	nulls = (bool *) palloc0(sizeof(bool) * nfields);

	for (i = 0; i < nfields; i++)
	{
		att = TupleDescAttr(tupdesc, i);
		nulls[i] = false;

		if (jl_is_tuple(obj))
			curr_elem = jl_get_nth_field(obj, i);
		else
		{
			char	   *attname = NameStr(att->attname);
			jl_value_t *key = jl_cstr_to_string(attname);
			jl_function_t *dict_get = jl_get_function(jl_main_module, "dict_get");

			curr_elem = jl_call2(dict_get, key, obj);
		}

		if (jl_typeis(curr_elem, jl_nothing_type))
		{
			nulls[i] = true;
			continue;
		}

		values[i] = jl_value_t_to_datum(current_call_data->fcinfo, curr_elem,
										att->atttypid, false);
	}
	tup = heap_form_tuple(tupdesc, values, nulls);
	pfree(nulls);
	pfree(values);
	return tup;
}

void
_PG_init(void)
{
	double		jl_init_time;
	struct timeval t1,
				t2;

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
	HASHCTL		hash_ctl;

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(pljulia_proc_key);
	hash_ctl.entrysize = sizeof(pljulia_hash_entry);

	pljulia_proc_hashtable =
		hash_create("PL/Julia cached procedures hashtable", 32, &hash_ctl,
					HASH_ELEM);

	char	   *dict_set_command,
			   *dict_get_command;

	/*
	 * The following functions are declared here and will be used to convert
	 * composite types to dictionaries. There might be a nicer way to do this
	 * using type constructors from Julia's C-API but it wasn't very obvious
	 * how.
	 */
	dict_set_command = "function dict_set(key, val, dict)\n"
		"dict[key] = val\n"
		"end";
	dict_get_command = "function dict_get(key, dict)\n"
		"if haskey(dict,key)\n"
		"return dict[key]\n"
		"else\n"
		"return nothing\n"
		"end\n"
		"end";
	/* add these functions to jl_main_module */
	jl_eval_string(dict_get_command);
	jl_eval_string(dict_set_command);
	jl_eval_string("init_nulls_anyarray(dims) = Array{Any}(nothing,dims)");
	jl_eval_string(
				   "return_next(arg) = ccall(:pljulia_return_next, Cvoid, (Any,), arg)");
	jl_eval_string("parse_bigfloat(arg) = parse(BigFloat, arg)");
	jl_eval_string("elog(level, message) = ccall(:pljulia_elog, Cvoid, "
				   "(Any,Any), level, message)");

}

/*
 * Convert the C string "input" to a Datum of type "typeoid".
 */
static Datum
cstring_to_type(char *input, Oid typeoid)
{
	Oid			typInput,
				typIOParam;
	Datum		ret;

	getTypeInputInfo(typeoid, &typInput, &typIOParam);
	ret = OidFunctionCall3(typInput, CStringGetDatum(input), 0, -1);

	PG_RETURN_DATUM(ret);
}

/*
 * Convert the Julia result to a Datum of type "typeoid".
 */
static Datum
jl_value_t_to_datum(FunctionCallInfo fcinfo, jl_value_t *ret, Oid prorettype, bool usefcinfo)
{
	/* maybe I should check the depth of the recursion stack */
	char	   *buffer;

	/* A nothing in Julia is a NULL in Postgres */
	if (jl_is_nothing(ret))
		PG_RETURN_VOID();
	/* Handle base types */
	if (jl_is_string(ret))
	{
		elog(DEBUG1, "ret (string): %s", jl_string_ptr(ret));
		PG_RETURN_DATUM(cstring_to_type((char *) jl_string_ptr(ret),
										prorettype));
	}
	else if (jl_typeis(ret, jl_float64_type))
	{
		double		ret_unboxed = jl_unbox_float64(ret);

		elog(DEBUG1, "ret (float64): %f", jl_unbox_float64(ret));

		buffer = (char *) palloc0((DOUBLE_LEN + 1) * sizeof(char));
		snprintf(buffer, DOUBLE_LEN, "%.16f", ret_unboxed);
	}
	else if (jl_is_bigfloat(ret))
	{
		/* this is how we map the numeric types */
		jl_function_t *str_func = jl_get_function(jl_base_module, "string");

		/*
		 * get the string representation of bigfloat since there's not a
		 * function in Julia's C-API for unboxing this type
		 */
		buffer = jl_string_ptr(jl_call1(str_func, ret));
	}
	else if (jl_typeis(ret, jl_float32_type))
	{
		double		ret_unboxed = jl_unbox_float32(ret);

		elog(DEBUG1, "ret (float32): %f", jl_unbox_float32(ret));

		buffer = (char *) palloc0((DOUBLE_LEN + 1) * sizeof(char));
		snprintf(buffer, DOUBLE_LEN, "%f", ret_unboxed);
	}
	else if (jl_typeis(ret, jl_int64_type))
	{
		long int	ret_unboxed = jl_unbox_int64(ret);

		elog(DEBUG1, "ret (int64): %ld", jl_unbox_int64(ret));

		buffer = (char *) palloc0((LONG_INT_LEN + 1) * sizeof(char));
		snprintf(buffer, LONG_INT_LEN, "%ld", ret_unboxed);
	}
	else if (jl_typeis(ret, jl_int32_type))
	{
		int			ret_unboxed = jl_unbox_int32(ret);

		elog(DEBUG1, "ret (int32): %d", jl_unbox_int32(ret));

		buffer = (char *) palloc0((LONG_INT_LEN + 1) * sizeof(char));
		snprintf(buffer, LONG_INT_LEN, "%d", ret_unboxed);
	}
	else if (jl_typeis(ret, jl_char_type))
	{
		char		ret_unboxed = jl_unbox_int32(ret);

		elog(DEBUG1, "ret (int32): %d", jl_unbox_int32(ret));

		buffer = (char *) palloc0((LONG_INT_LEN + 1) * sizeof(char));
		snprintf(buffer, LONG_INT_LEN, "%c", ret_unboxed);
	}
	else if (jl_typeis(ret, jl_bool_type))
	{
		int			ret_unboxed = jl_unbox_bool(ret);

		elog(DEBUG1, "ret (bool): %d", jl_unbox_bool(ret));

		buffer = (char *) palloc0((LONG_INT_LEN + 1) * sizeof(char));
		snprintf(buffer, LONG_INT_LEN, "%d", ret_unboxed);
	}
	/* If not a base type, but still a valid type */
	else if (jl_is_array(ret))
	{
		/* handle the arraytype */
		PG_RETURN_DATUM(pg_array_from_julia_array(fcinfo, ret, prorettype));
	}
	else if (jl_is_tuple(ret))
	{
		/* handle the tupletype - return a composite */
		PG_RETURN_DATUM(pg_composite_from_julia_tuple(fcinfo, ret, prorettype, usefcinfo));
	}
	else if (jl_is_dict(ret))
	{
		PG_RETURN_DATUM(pg_composite_from_julia_dict(fcinfo, ret, prorettype, usefcinfo));
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
					   pljulia_proc_desc *prodesc)
{
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	char	   *value;
	int			i;
	Form_pg_type type_struct;
	HeapTuple	type_tuple;
	MemoryContext proc_cxt;
	bool		is_array_type;

	get_func_arg_info(procedure_tuple, &argtypes, &argnames, &argmodes);
	proc_cxt = prodesc->mcxt;

	for (i = 0; i < fcinfo->nargs; i++)
	{
		Oid			argtype = procedure_struct->proargtypes.values[i];

		type_tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(argtype));
		if (!HeapTupleIsValid(type_tuple))
			elog(ERROR, "cache lookup failed for type %u", argtype);

		prodesc->arg_is_rowtype[i] = type_is_rowtype(argtypes[i]);

		type_struct = (Form_pg_type) GETSTRUCT(type_tuple);
		if (!prodesc->arg_is_rowtype[i])
			fmgr_info_cxt(type_struct->typoutput, &(prodesc->arg_out_func[i]),
						  proc_cxt);
		/* Whether it's a "true" array type */
		is_array_type = (type_struct->typelem != 0 &&
						 type_struct->typlen == -1);
		prodesc->arg_arraytype[i] = (is_array_type) ? argtypes[i] : InvalidOid;

		ReleaseSysCache(type_tuple);
		/* First check if the input argument is NULL */
		if (fcinfo->args[i].isnull)
		{
			boxed_args[i] = jl_nothing;
			continue;
		}
		boxed_args[i] = convert_arg_to_julia(fcinfo->args[i].value,
											 argtypes[i], prodesc, i);

		elog(DEBUG1, "[%d] %s = %s :: %u", i, argnames[i], value, argtypes[i]);
	}
}

jl_value_t *
convert_arg_to_julia(Datum d, Oid argtype, pljulia_proc_desc *prodesc, int i)
{
	jl_value_t *result;
	bool		is_array_type = (prodesc->arg_arraytype[i] != InvalidOid);

	if (is_array_type)
		result = julia_array_from_datum(d, argtype);
	else if (prodesc->arg_is_rowtype[i])
	{
		result = julia_dict_from_datum(d);
	}
	else
	{
		char	   *value;

		value = OutputFunctionCall(&prodesc->arg_out_func[i], d);
		result = pg_oid_to_jl_value(argtype, value);
	}

	return result;
}

jl_value_t *
julia_dict_from_datum(Datum d)
{
	HeapTupleHeader td;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc = NULL;
	HeapTupleData tmptup;
	jl_value_t *ret;

	td = DatumGetHeapTupleHeader(d);
	/* Extract rowtype info and find a tupdesc */
	tupType = HeapTupleHeaderGetTypeId(td);
	tupTypmod = HeapTupleHeaderGetTypMod(td);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
	/* Build a temporary HeapTuple control structure */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
	tmptup.t_data = td;

	ret = pljulia_dict_from_tuple(&tmptup, tupdesc, true);
	ReleaseTupleDesc(tupdesc);
	return ret;
}

jl_value_t *
pljulia_dict_from_tuple(HeapTuple tuple, TupleDesc tupdesc,
						bool include_generated)
{
	int			i;
	Form_pg_attribute att;
	char	   *attname;
	Datum		attr;
	bool		isnull;
	Oid			typoutput;
	bool		typisvarlena;
	jl_value_t *dict,
			   *key,
			   *value;
	jl_function_t *dict_set;

	/* dict_set(key, value, dict) */
	dict_set = jl_get_function(jl_main_module, "dict_set");
	/* create an empty dictionary ({Any, Any}) */
	dict = jl_eval_string("Dict()");

	for (i = 0; i < tupdesc->natts; i++)
	{
		att = TupleDescAttr(tupdesc, i);
		/* ignore dropped */
		if (att->attisdropped)
			continue;
		if (att->attgenerated && !include_generated)
			continue;
		/* get attribute name */
		attname = NameStr(att->attname);
		key = jl_cstr_to_string(attname);
		/* get its value as Datum */
		attr = heap_getattr(tuple, i + 1, tupdesc, &isnull);
		getTypeOutputInfo(att->atttypid, &typoutput, &typisvarlena);

		if (isnull)
		{
			/* just do dict[attname] = nothing */
			jl_call3(dict_set, key, (jl_value_t *) jl_nothing, dict);
			continue;
		}

		/*
		 * not NULL: convert the value to its julia representation first, then
		 * call dict_set to insert it in the dictionary. For now assume all
		 * fields of the tuple will be base types.
		 */
		else
		{
			char	   *outputstr;

			outputstr = OidOutputFunctionCall(typoutput, attr);
			value = pg_oid_to_jl_value(att->atttypid, outputstr);
			jl_call3(dict_set, key, (jl_value_t *) value, dict);
		}
	}
	return dict;
}

jl_value_t *
julia_array_from_datum(Datum d, Oid argtype)
{
	ArrayType  *ar;
	Oid			elementtype,
				typioparam,
				typoutputfunc;
	int16		typlen;
	bool		typbyval;
	char		typalign,
				typdelim;
	int			i,
				j,
				nitems,
				ndims,
			   *dims;
	bool	   *nulls;
	Datum	   *elements;
	jl_array_t *jl_arr;
	char	   *value;
	jl_value_t **types,
			  **tupvalues;		/* types: the dimension types, so all int64
								 * tupvalues: the size of each dimension  */
	jl_tupletype_t *tt;
	jl_function_t *init_arr;
	jl_value_t *jl_boxed_elem,
			   *dimtuple;
	FmgrInfo   *arg_out_func;
	Form_pg_type type_struct;
	HeapTuple	type_tuple;

	arg_out_func = (FmgrInfo *) palloc0(sizeof(FmgrInfo));
	ar = DatumGetArrayTypeP(d);
	elementtype = ARR_ELEMTYPE(ar);
	ndims = ARR_NDIM(ar);
	dims = ARR_DIMS(ar);

	types = (jl_value_t **) palloc0(ndims * sizeof(jl_value_t *));
	tupvalues = (jl_value_t **) palloc0(ndims * sizeof(jl_value_t *));

	get_typlenbyvalalign(elementtype, &typlen, &typbyval, &typalign);

	/* get datum representation of each array element */
	deconstruct_array(ar, elementtype, typlen, typbyval, typalign, &elements,
					  &nulls, &nitems);
	/* elements[i] is a datum */

	/* get the conversion function from Datum to elementtype */
	type_tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(elementtype));
	if (!HeapTupleIsValid(type_tuple))
		elog(ERROR, "cache lookup failed for type %u", argtype);

	type_struct = (Form_pg_type) GETSTRUCT(type_tuple);
	fmgr_info(type_struct->typoutput, arg_out_func);
	ReleaseSysCache(type_tuple);

	for (i = 0; i < ndims; i++)
		types[i] = (jl_value_t *) jl_int64_type;

	tt = jl_apply_tuple_type_v(types, ndims);

	for (i = 0; i < ndims; i++)
		tupvalues[i] = jl_box_int64(dims[i]);

	dimtuple = jl_new_structv(tt, tupvalues, ndims);
	init_arr = jl_get_function(jl_main_module, "init_nulls_anyarray");
	jl_arr = jl_call1(init_arr, dimtuple);

	for (i = 0; i < nitems; i++)
	{
		j = calculate_cm_offset(i, ndims, dims);
		/* Check whether null */
		if (nulls[i])
		{
			/* already initialized to nothing so this is redundant */
			jl_arrayset(jl_arr, (jl_value_t *) jl_nothing, j);
			continue;
		}
		value = OutputFunctionCall(arg_out_func, elements[i]);

		jl_boxed_elem = pg_oid_to_jl_value(elementtype, value);
		jl_arrayset(jl_arr, (jl_value_t *) jl_boxed_elem, j);
	}
	return (jl_value_t *) jl_arr;
}

Datum
pljulia_inline_handler(PG_FUNCTION_ARGS)
{
	/*
	 * An inline code block is basically a function that doesn't take input
	 * arguments and does not return anything (void)
	 */
	InlineCodeBlock *codeblock = (InlineCodeBlock *) PG_GETARG_POINTER(0);
	char	   *source_code = codeblock->source_text;

	jl_eval_string(source_code);
	if (jl_exception_occurred())
		elog(ERROR, "%s",
			 jl_string_ptr(jl_eval_string(
										  "sprint(showerror, ccall(:jl_exception_occurred, Any, ()))")));

	PG_RETURN_VOID();
}

/*
 * Handle function, procedure, and trigger calls.
 */
Datum
pljulia_call_handler(PG_FUNCTION_ARGS)
{
	Datum		ret;
	pljulia_call_data *volatile save_call_data = current_call_data;
	pljulia_call_data this_call_data;

	/* Initialize current-call status record */
	MemSet(&this_call_data, 0, sizeof(this_call_data));
	this_call_data.fcinfo = fcinfo;

	current_call_data = &this_call_data;
	/* run Julia code */
	if (CALLED_AS_TRIGGER(fcinfo))
		ret = pljulia_trigger_handler(fcinfo);
	else if (CALLED_AS_EVENT_TRIGGER(fcinfo))
	{
		pljulia_event_trigger_handler(fcinfo);
		ret = (Datum) 0;
	}
	else
		ret = pljulia_execute(fcinfo);

	current_call_data = save_call_data;

	/*
	 * strongly recommended: notify Julia that the program is about to
	 * terminate. this allows Julia time to cleanup pending write requests and
	 * run all finalizers
	 */
	/* jl_atexit_hook(0); */

	return ret;
}

/*
 * Retrieve Julia code and create a user-defined function
 * with a unique name.
 */
pljulia_proc_desc *
pljulia_compile(FunctionCallInfo fcinfo, HeapTuple procedure_tuple,
				Form_pg_proc procedure_struct, bool is_trigger,
				bool is_event_trigger)
{
	Datum		procedure_source_datum;
	const char *procedure_code;
	bool		isnull;
	volatile MemoryContext proc_cxt = NULL;
	MemoryContext oldcontext;

	int			compiled_len = 0;
	char	   *compiled_code;
	pljulia_proc_desc *prodesc = NULL;

	int			i;
	FmgrInfo   *arg_out_func;
	Form_pg_type type_struct;
	HeapTuple	type_tuple;

	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	char	   *value;

	bool		found_hashentry;
	pljulia_proc_key proc_key;
	pljulia_hash_entry *hash_entry;

	/* First try to find the function in the lookup table */
	proc_key.fn_oid = fcinfo->flinfo->fn_oid;
	proc_key.is_trigger = is_trigger;
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
			 * found_hashentry is redundant since we don't expect a returned
			 * entry from HASH_REMOVE
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
	char		internal_procname[256];

	procedure_source_datum = SysCacheGetAttr(PROCOID, procedure_tuple,
											 Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc");

	procedure_code = DatumGetCString(DirectFunctionCall1(textout,
														 procedure_source_datum));
	elog(DEBUG1, "procedure code:\n%s", procedure_code);

	/* if it's a regular function call */
	if (!is_trigger && !is_event_trigger)
	{
		/*
		 * Add the final carriage return to the length of the original
		 * procedure.
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
		 * Loop through the parameters to determine how big of a buffer is
		 * needed for prepending the parameter names as input arguments to the
		 * function/procedure code.
		 */
		for (i = 0; i < fcinfo->nargs; i++)
		{
			Oid			argtype = procedure_struct->proargtypes.values[i];

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
		 * Declare the procedure code as a function with the input parameters
		 * as the function arguments.
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
		prodesc->fn_retisset = procedure_struct->proretset;
		prodesc->fn_retistuple = type_is_rowtype(procedure_struct->prorettype);
		prodesc->mcxt = proc_cxt;
		prodesc->fn_xmin = HeapTupleHeaderGetRawXmin(procedure_tuple->t_data);
		/* this is filled later on when handling the input args */
		prodesc->arg_out_func = (FmgrInfo *) palloc0(prodesc->nargs *
													 sizeof(FmgrInfo));
		prodesc->arg_arraytype = (Oid *) palloc0(prodesc->nargs * sizeof(Oid));
		prodesc->arg_is_rowtype = (bool *) palloc0(prodesc->nargs * sizeof(bool));
		MemoryContextSwitchTo(oldcontext);
	}
	else if (is_trigger)
	{
		/* the following arguments are standard for trigger calls */
		char	   *TD_standard_args =
		"TD_name, TD_relid, TD_table_name, TD_table_schema, TD_event, TD_when, "
		"TD_level, TD_NEW, TD_OLD, args";

		compiled_len += strlen(procedure_code) + 1;
		proc_cxt = AllocSetContextCreate(TopMemoryContext, "PL/Julia function",
										 ALLOCSET_SMALL_SIZES);
		snprintf(internal_procname, sizeof(internal_procname), "pljulia_%u",
				 fcinfo->flinfo->fn_oid);
		/* +1 is for the line break (\n) */
		compiled_len += strlen("function ") + 1;
		i = strlen(internal_procname);
		compiled_len += i;
		internal_procname[i] = '\0';
		/* one \n and '(' and ')' */
		compiled_len += strlen("end") + 3;
		compiled_len += strlen(TD_standard_args);

		oldcontext = MemoryContextSwitchTo(proc_cxt);
		compiled_code = (char *) palloc0(compiled_len * sizeof(char));

		compiled_code[0] = '\0';
		strcpy(compiled_code, "function ");
		strcat(compiled_code, internal_procname);
		/* now the argument names */
		strcat(compiled_code, "(");
		strcat(compiled_code, TD_standard_args);
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
		prodesc->mcxt = proc_cxt;
		MemoryContextSwitchTo(oldcontext);
	}
	else if (is_event_trigger)
	{
		/* the following arguments are standard for event trigger calls */
		char	   *TD_standard_args = "TD_event, TD_tag";

		compiled_len += strlen(procedure_code) + 1;
		proc_cxt = AllocSetContextCreate(TopMemoryContext, "PL/Julia function",
										 ALLOCSET_SMALL_SIZES);
		snprintf(internal_procname, sizeof(internal_procname), "pljulia_%u",
				 fcinfo->flinfo->fn_oid);
		/* +1 is for the line break (\n) */
		compiled_len += strlen("function ") + 1;
		i = strlen(internal_procname);
		compiled_len += i;
		internal_procname[i] = '\0';
		/* one \n and '(' and ')' */
		compiled_len += strlen("end") + 3;
		compiled_len += strlen(TD_standard_args);

		oldcontext = MemoryContextSwitchTo(proc_cxt);
		compiled_code = (char *) palloc0(compiled_len * sizeof(char));

		compiled_code[0] = '\0';
		strcpy(compiled_code, "function ");
		strcat(compiled_code, internal_procname);
		/* now the argument names */
		strcat(compiled_code, "(");
		strcat(compiled_code, TD_standard_args);
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
		prodesc->mcxt = proc_cxt;
		MemoryContextSwitchTo(oldcontext);
	}
	/* Create a new hashtable entry for the new function definition */
	hash_entry = hash_search(pljulia_proc_hashtable, &proc_key, HASH_ENTER,
							 &found_hashentry);
	if (hash_entry == NULL)
		elog(ERROR, "pljulia: hash table out of memory");
	hash_entry->prodesc = prodesc;

	/* insert function declaration into Julia */
	jl_eval_string(compiled_code);
	if (jl_exception_occurred())
		show_julia_error();

	return prodesc;
}

/*
 * Execute Julia code and handle the data returned by Julia.
 */
static Datum
pljulia_execute(FunctionCallInfo fcinfo)
{
	jl_value_t **boxed_args;
	HeapTuple	procedure_tuple;
	Form_pg_proc procedure_struct;
	jl_value_t *ret;
	jl_function_t *func;
	Datum		retval;
	ReturnSetInfo *rsi;

	pljulia_proc_desc *prodesc = NULL;

	rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	procedure_tuple = SearchSysCache(PROCOID,
									 ObjectIdGetDatum(fcinfo->flinfo->fn_oid), 0, 0, 0);
	if (!HeapTupleIsValid(procedure_tuple))
		elog(ERROR, "cache lookup failed for function %u",
			 fcinfo->flinfo->fn_oid);
	procedure_struct = (Form_pg_proc) GETSTRUCT(procedure_tuple);

	/* function definition + body code */
	prodesc = pljulia_compile(fcinfo, procedure_tuple, procedure_struct, false,
							  false);
	current_call_data->prodesc = prodesc;
	ReleaseSysCache(procedure_tuple);

	/*
	 * this assumes that pljulia_compile inserts the function definition into
	 * the interpreter
	 */
	func = jl_get_function(jl_main_module, prodesc->internal_proname);

	if (jl_exception_occurred())
		show_julia_error();

	/*
	 * insert the function code into the julia interpreter then get a pointer
	 * to the function and call it after the arguments are settled too .
	 * Allocate the space for the arguments here
	 */
	boxed_args = (jl_value_t **) palloc0(prodesc->nargs * sizeof(jl_value_t *));

	julia_setup_input_args(fcinfo, procedure_tuple, procedure_struct,
						   boxed_args, prodesc);
	if (jl_exception_occurred())
		show_julia_error();
	ret = jl_call(func, boxed_args, prodesc->nargs);

	if (jl_exception_occurred())
		show_julia_error();

	/*
	 * if fn_retisset handle differently... jl_value_t_to_datum only if we
	 * have a non-srf
	 */

	if (prodesc->fn_retisset)
	{
		rsi->returnMode = SFRM_Materialize;
		/* if we have any tuples to return */
		if (current_call_data->tuple_store)
		{
			rsi->setResult = current_call_data->tuple_store;
			rsi->setDesc = current_call_data->ret_tupdesc;
		}
		/* equivalent to PG_RERTURN_NULL */
		retval = (Datum) 0;
		fcinfo->isnull = true;
	}
	else
	{
		retval = jl_value_t_to_datum(fcinfo, ret, procedure_struct->prorettype,
									 true);
	}
	return retval;
}

Datum
pg_array_from_julia_array(FunctionCallInfo fcinfo, jl_value_t *ret,
						  Oid prorettype)
{
	ArrayType  *array;
	Datum	   *array_elem;
	bool	   *nulls = NULL;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	int			row_major_offset;
	Oid			elem_type = get_element_type(prorettype);
	size_t		len = jl_array_len(ret);
	int			ndim = jl_array_ndims(ret);
	int		   *dims = (int *) palloc0(sizeof(int) * ndim);
	int		   *lbs = (int *) palloc0(sizeof(int) * ndim);
	int			i;
	jl_value_t *curr_elem;

	for (i = 0; i < ndim; i++)
	{
		dims[i] = jl_array_dim(ret, i);
		lbs[i] = 1;
	}
	elog(DEBUG1, "len : %zu\n", len);

	array_elem = (Datum *) palloc0(sizeof(Datum) * len);

	for (i = 0; i < len; i++)
	{
		row_major_offset = calculate_rm_offset(i, ndim, dims);
		curr_elem = jl_arrayref(ret, i);
		/* if jl_nothing then set it as NULL */
		if (jl_typeis(curr_elem, jl_nothing_type))
		{
			/* first null we found, so palloc */
			if (!nulls)
				nulls = (bool *) palloc0(sizeof(bool) * len);

			nulls[i] = true;
			continue;
		}

		array_elem[row_major_offset] = jl_value_t_to_datum(fcinfo, curr_elem,
														   elem_type, false);
		/* if for some reason it wasn't possible to */
	}
	get_typlenbyvalalign(elem_type, &typlen, &typbyval, &typalign);
	array = construct_md_array(array_elem, nulls, ndim, dims, lbs, elem_type,
							   typlen, typbyval, typalign);
	PG_RETURN_ARRAYTYPE_P(array);
}

Datum
pg_composite_from_julia_tuple(FunctionCallInfo fcinfo, jl_value_t *ret,
							  Oid prorettype, bool usefcinfo)
{
	int			nfields,
				i;
	jl_value_t *curr_elem;
	TupleDesc	tupdesc;
	Oid			resultTypeId;
	Datum	   *elements;
	bool	   *nulls = NULL;
	HeapTuple	tup;
	Form_pg_attribute att;

	if (usefcinfo)
	{
		if (get_call_result_type(fcinfo, &resultTypeId, &tupdesc) !=
			TYPEFUNC_COMPOSITE)
			elog(ERROR, "function returning record called in context that cannot "
				 "accept type record");
	}

	/*
	 * if !usefcinfo then don't use it to get a tupdesc because we've been
	 * called from pg_array_from_julia_array and fcinfo concerns the array,
	 * not the tuple we need to build
	 */
	else
	{
		/*
		 * set typmod -1 because we don't expect a domain. For domains extra
		 * work needs to be done
		 */
		tupdesc = lookup_rowtype_tupdesc(prorettype, -1);
	}

	nfields = jl_nfields(ret);
	if (tupdesc->natts != nfields)
		elog(ERROR, "Tuple number of fields mismatch");

	elements = (Datum *) palloc0(sizeof(Datum) * nfields);
	nulls = (bool *) palloc0(sizeof(bool) * nfields);

	for (i = 0; i < nfields; i++)
	{
		curr_elem = jl_get_nth_field(ret, i);
		if (jl_typeis(curr_elem, jl_nothing_type))
		{
			nulls[i] = true;
			continue;
		}
		nulls[i] = false;
		att = TupleDescAttr(tupdesc, i);

		elements[i] = jl_value_t_to_datum(fcinfo, curr_elem, att->atttypid, false);
	}
	tup = heap_form_tuple(tupdesc, elements, nulls);
	ReleaseTupleDesc(tupdesc);
	return HeapTupleGetDatum(tup);
}

Datum
pg_composite_from_julia_dict(FunctionCallInfo fcinfo, jl_value_t *ret,
							 Oid prorettype, bool usefcinfo)
{
	int			nfields,
				i;
	jl_value_t *curr_elem,
			   *key;
	TupleDesc	tupdesc;
	Oid			resultTypeId;
	Datum	   *elements;
	bool	   *nulls = NULL;
	HeapTuple	tup;
	char	   *attname;
	Form_pg_attribute att;
	jl_function_t *dict_get,
			   *dict_nfields;

	if (usefcinfo)
	{
		if (get_call_result_type(fcinfo, &resultTypeId, &tupdesc) !=
			TYPEFUNC_COMPOSITE)
			elog(ERROR,
				 "function returning record called in context that cannot "
				 "accept type record");
	}

	/*
	 * if !usefcinfo then don't use it to get a tupdesc because we've been
	 * called from pg_array_from_julia_array and fcinfo concerns the array,
	 * not the tuple we need to build
	 */
	else
	{
		/*
		 * set typmod -1 because we don't expect a domain. For domains extra
		 * work needs to be done
		 */
		tupdesc = lookup_rowtype_tupdesc(prorettype, -1);
	}

	/* the number of entries in the dictionary equals its length */
	dict_nfields = jl_get_function(jl_base_module, "length");
	dict_get = jl_get_function(jl_main_module, "dict_get");
	nfields = jl_unbox_int64(jl_call1(dict_nfields, ret));
	if (tupdesc->natts != nfields)
		elog(ERROR, "Dict number of fields mismatch");

	elements = (Datum *) palloc0(sizeof(Datum) * nfields);
	nulls = (bool *) palloc0(sizeof(bool) * nfields);

	for (i = 0; i < nfields; i++)
	{
		att = TupleDescAttr(tupdesc, i);
		attname = NameStr(att->attname);
		key = jl_cstr_to_string(attname);
		curr_elem = jl_call2(dict_get, key, ret);
		if (jl_typeis(curr_elem, jl_nothing_type))
		{
			nulls[i] = true;
			continue;
		}
		nulls[i] = false;

		elements[i] = jl_value_t_to_datum(fcinfo, curr_elem, att->atttypid,
										  false);
	}
	tup = heap_form_tuple(tupdesc, elements, nulls);
	ReleaseTupleDesc(tupdesc);
	return HeapTupleGetDatum(tup);
}

static Datum
pljulia_trigger_handler(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	TupleDesc	tupdesc;
	HeapTuple	procedure_tuple;
	Form_pg_proc procedure_struct;
	volatile HeapTuple rettuple;
	int			i,
				rc;
	pljulia_proc_desc *prodesc;
	jl_value_t *trig_args[10];
	char	   *stroid,
			   *when,
			   *level,
			   *event,
			   *arg;
	jl_function_t *func,
			   *init_arr;
	jl_value_t *ret;

	/* make sure we're here from a call to a trigger */
	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "not called by trigger manager");
	/* connect to the SPI manager */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI manager");
	/* make transition tables visible to this SPI connection */
	rc = SPI_register_trigger_data(trigdata);

	if (rc < 0)
		elog(ERROR,
			 "Could not make transition tables visible to PL trigger handler");

	procedure_tuple = SearchSysCache(PROCOID,
									 ObjectIdGetDatum(fcinfo->flinfo->fn_oid),
									 0, 0, 0);
	if (!HeapTupleIsValid(procedure_tuple))
		elog(ERROR, "cache lookup failed for function %u",
			 fcinfo->flinfo->fn_oid);
	procedure_struct = (Form_pg_proc) GETSTRUCT(procedure_tuple);

	prodesc = pljulia_compile(fcinfo, procedure_tuple, procedure_struct,
							  true, false);
	current_call_data->prodesc = prodesc;
	ReleaseSysCache(procedure_tuple);
	tupdesc = RelationGetDescr(trigdata->tg_relation);

	/*
	 * setup trigger args, first the standard ones, 10 in total  TD_name,
	 * TD_relid, TD_table_name, TD_table_schema, TD_event, TD_when TD_level,
	 * TD_NEW, TD_OLD, args, where args will be passed as an array
	 */

	/* set up TD_name with trigger name */
	trig_args[0] = jl_cstr_to_string(utf_e2u(trigdata->tg_trigger->tgname));
	/* TD_relid */
	stroid = DatumGetCString(
							 DirectFunctionCall1(oidout,
												 ObjectIdGetDatum(trigdata->tg_relation->rd_id)));
	trig_args[1] = jl_cstr_to_string(stroid);
	pfree(stroid);

	/* TD_table_name */
	stroid = SPI_getrelname(trigdata->tg_relation);
	trig_args[2] = jl_cstr_to_string(utf_e2u(stroid));
	pfree(stroid);

	/* TD_table_schema */
	stroid = SPI_getnspname(trigdata->tg_relation);
	trig_args[3] = jl_cstr_to_string(utf_e2u(stroid));
	pfree(stroid);

	/* TD_event */
	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		event = "INSERT";
	else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
		event = "DELETE";
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		event = "UPDATE";
	else if (TRIGGER_FIRED_BY_TRUNCATE(trigdata->tg_event))
		event = "TRUNCATE";
	else
		elog(ERROR, "unrecognized OPERATION tg_event: %u", trigdata->tg_event);

	trig_args[4] = jl_cstr_to_string(event);

	/* TD_when, can be BEFORE, AFTER, INSTEAD OF */
	if (TRIGGER_FIRED_BEFORE(trigdata->tg_event))
		when = "BEFORE";
	else if (TRIGGER_FIRED_AFTER(trigdata->tg_event))
		when = "AFTER";
	else if (TRIGGER_FIRED_INSTEAD(trigdata->tg_event))
		when = "INSTEAD OF";
	else
		elog(ERROR, "unrecognized WHEN tg_event: %u", trigdata->tg_event);
	trig_args[5] = jl_cstr_to_string(when);
	/* TD_level */
	if (TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		level = "ROW";
	else if (TRIGGER_FIRED_FOR_STATEMENT(trigdata->tg_event))
		level = "STATEMENT";
	else
		elog(ERROR, "unrecognized LEVEL tg_event: %u", trigdata->tg_event);
	trig_args[6] = jl_cstr_to_string(level);

	/* TD_new and TD_old */

	/*
	 * TD_new and TD_old are initialized NULL (Julia nothing), and depending
	 * on whether we have a trigger function that modifies rows, we set the
	 * appropriate values for NEW/OLD rows
	 */

	/* TD_new */
	trig_args[7] = jl_nothing;
	/* TD_OLD */
	trig_args[8] = jl_nothing;

	rettuple = (HeapTuple) NULL;

	if (TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
	{
		/* we only have a new row to return in the case of INSERT */
		if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		{
			trig_args[7] = pljulia_dict_from_tuple(trigdata->tg_trigtuple,
												   tupdesc, !TRIGGER_FIRED_BEFORE(trigdata->tg_event));
			rettuple = trigdata->tg_trigtuple;
		}

		/* we only have an old row in the case of DELETE */
		else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
		{
			trig_args[8] = pljulia_dict_from_tuple(trigdata->tg_trigtuple,
												   tupdesc, true);
			rettuple = trigdata->tg_trigtuple;
		}

		/* we have both a new and an old row */
		else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		{
			trig_args[7] = pljulia_dict_from_tuple(trigdata->tg_newtuple,
												   tupdesc, !TRIGGER_FIRED_BEFORE(trigdata->tg_event));
			trig_args[8] = pljulia_dict_from_tuple(trigdata->tg_trigtuple,
												   tupdesc, true);
			rettuple = trigdata->tg_newtuple;
		}
	}
	/* Finally, setup any args to the trigger in an array */
	init_arr = jl_get_function(jl_main_module, "init_nulls_anyarray");
	trig_args[9] = jl_call1(init_arr, jl_box_int64(trigdata->tg_trigger->tgnargs));

	/*
	 * All arguments are passed as strings to the Julia function, and it's up
	 * to the user to convert to the desired type if needed (for example,
	 * using parse for numeric input).
	 */
	for (i = 0; i < trigdata->tg_trigger->tgnargs; i++)
	{
		arg = utf_e2u(trigdata->tg_trigger->tgargs[i]);
		jl_arrayset(trig_args[9],
					jl_cstr_to_string(arg), i);
	}
	/* Now call the trigger function */
	func = jl_get_function(jl_main_module, prodesc->internal_proname);

	ret = jl_call(func, trig_args, 10);
	if (jl_exception_occurred())
		show_julia_error();

	/* Disconnect from the SPI_manager */
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish() failed");

	/*
	 * If function returned nothing or "OK", proceed normally. If function
	 * returned "SKIP", then suppress the operation for this row. Otherwise,
	 * for a "MODIFY" operation, the returned value must be a dictionary, with
	 * rownames as keys and the desired corresponding values.
	 */
	if (jl_is_nothing(ret))
		return PointerGetDatum(rettuple);
	else if (jl_is_string(ret))
	{
		if (strcmp("OK", jl_string_ptr(ret)) == 0)
			return PointerGetDatum(rettuple);
		else if (strcmp("SKIP", jl_string_ptr(ret)) == 0)
		{
			rettuple = (HeapTuple) NULL;
			return PointerGetDatum(rettuple);
		}
		else
			elog(ERROR, "Trigger function must return either nothing, \"OK\", "
				 "\"SKIP\" or a dictionary corresponding to the new tuple");
	}
	else if (jl_is_dict(ret))
	{
		/* Create the modified tuple to return */
		rettuple = pljulia_build_tuple_result(ret,
											  trigdata->tg_relation->rd_att);

		/*
		 * Still need to check if the operation was an INSERT or UPDATE
		 * Otherwise, returning a modified row doesn't make sense
		 */
		if (!TRIGGER_FIRED_BY_INSERT(trigdata->tg_event) && !TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		{
			/* Perhaps another error level ? */
			elog(NOTICE, "Ignoring modified row, not an INSERT or UPDATE");
			rettuple = trigdata->tg_trigtuple;
		}

	}
	else
		elog(ERROR, "Trigger function must return either nothing, \"OK\", "
			 "\"SKIP\" or a dictionary corresponding to the new tuple");

	return PointerGetDatum(rettuple);
}

static void
pljulia_event_trigger_handler(PG_FUNCTION_ARGS)
{
	pljulia_proc_desc *prodesc;
	EventTriggerData *trigdata = (EventTriggerData *) fcinfo->context;
	int			rc;
	HeapTuple	procedure_tuple;
	Form_pg_proc procedure_struct;
	jl_value_t *trig_args[2];
	jl_function_t *func;

	/* make sure we're here from a call to an event trigger */
	if (!CALLED_AS_EVENT_TRIGGER(fcinfo))
		elog(ERROR, "not called by trigger manager");
	/* connect to the SPI manager */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "could not connect to SPI manager");

	procedure_tuple = SearchSysCache(PROCOID,
									 ObjectIdGetDatum(fcinfo->flinfo->fn_oid),
									 0, 0, 0);
	if (!HeapTupleIsValid(procedure_tuple))
		elog(ERROR, "cache lookup failed for function %u",
			 fcinfo->flinfo->fn_oid);
	procedure_struct = (Form_pg_proc) GETSTRUCT(procedure_tuple);

	prodesc = pljulia_compile(fcinfo, procedure_tuple, procedure_struct,
							  false, true);
	ReleaseSysCache(procedure_tuple);

	/* TD_event */
	trig_args[0] = jl_cstr_to_string(utf_e2u(trigdata->event));
	/* TD_tag */
	trig_args[1] = jl_cstr_to_string(utf_e2u(GetCommandTagName(trigdata->tag)));

	func = jl_get_function(jl_main_module, prodesc->internal_proname);
	/* the value returned by an event trigger is ignored */
	jl_call2(func, trig_args[0], trig_args[1]);
	if (jl_exception_occurred())
		show_julia_error();

	/* disconnect from the SPI manager */
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish() failed");

}

PG_FUNCTION_INFO_V1(pljulia_validator);

Datum
pljulia_validator(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Form_pg_proc proc;
	char	   *code;
	Datum		prosrc_datum;
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	bool		isnull;
	char		functyptype;
	bool		is_trigger = false,
				is_event_trigger = false;
	int			i,
				nargs;
	int			compiled_len = 0;
	char	   *trig_args =
	"TD_name, TD_relid, TD_table_name, TD_table_schema, TD_event, TD_when, "
	"TD_level, TD_NEW, TD_OLD, args";
	char	   *evt_trig_args = "TD_event, TD_tag";
	char	   *compiled_code;

	/*
	 * Verify that we have a pljulia function and that the user has access to
	 * both the language and the function
	 */
	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, funcoid))
		PG_RETURN_VOID();
	/* the user might not require validation */
	if (!check_function_bodies)
	{
		elog(NOTICE, "check_function_bodies is disabled, skipping validation");
		PG_RETURN_VOID();
	}

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	functyptype = get_typtype(proc->prorettype);

	/* Disallow pseudotype result */
	/* except for TRIGGER, EVTTRIGGER, RECORD, or VOID */
	if (functyptype == TYPTYPE_PSEUDO)
	{
		if (proc->prorettype == TRIGGEROID)
			is_trigger = true;
		else if (proc->prorettype == EVTTRIGGEROID)
			is_event_trigger = true;
		else if (proc->prorettype != RECORDOID &&
				 proc->prorettype != VOIDOID)
			elog(ERROR,
				 errmsg("PL/Julia functions cannot return type %s",
						format_type_be(proc->prorettype)));
	}

	prosrc_datum = SysCacheGetAttr(PROCOID, tuple,
								   Anum_pg_proc_prosrc, &isnull);
	code = DatumGetCString(DirectFunctionCall1(textout,
											   prosrc_datum));
	nargs = get_func_arg_info(tuple, &argtypes, &argnames, &argmodes);

	/*
	 * if there's another user function with the name pljulia_validate_func
	 * this shouldn't cause an error
	 */
	compiled_len += strlen("function pljulia_validate_func()\n\nend");

	/*
	 * simply prepend the arguments to the source code and then call
	 * jl_eval_string
	 */
	if (is_trigger)
		compiled_len += strlen(trig_args);
	else if (is_event_trigger)
		compiled_len += strlen(evt_trig_args);
	else
	{
		for (i = 0; i < nargs; i++)
		{
			/* +1 for the ',' */
			compiled_len += strlen(argnames[i]) + 1;
		}
	}
	compiled_len += strlen(code);
	compiled_code = (char *) palloc0(compiled_len * sizeof(char));
	compiled_code[0] = '\0';
	strcpy(compiled_code, "function pljulia_validate_func");
	strcat(compiled_code, "(");
	if (is_trigger)
	{
		strcat(compiled_code, trig_args);
		strcat(compiled_code, ")");
	}
	else if (is_event_trigger)
	{
		strcat(compiled_code, evt_trig_args);
		strcat(compiled_code, ")");
	}
	else
	{
		/* now the argument names */
		for (i = 0; i < nargs; i++)
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
	}

	strcat(compiled_code, code);
	strcat(compiled_code, "\nend");

	jl_eval_string(compiled_code);
	if (jl_exception_occurred())
		show_julia_error();
	ReleaseSysCache(tuple);

	/* The validator's result is ignored in any case */
	PG_RETURN_VOID();
}
