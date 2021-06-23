#include "convert_args.h"

jl_value_t *
pg_oid_to_jl_value(Oid argtype, const char *value)
{
	jl_value_t *result;
	switch (argtype)
	{
	case INT2OID:
	case INT4OID:
		/*
		 * should be jl_box_int32 but this will be taken care of along with the
		 * rest of the input conversion. For a very little while let's leave it
		 * like this and focus on caching the procedure code working with just
		 * some input types
		 */
		result = jl_box_int64(atoi(value));
		break;

	case INT8OID:
		result = jl_box_int64(atoi(value));
		break;

	case FLOAT4OID:
		result = jl_box_float32(atof(value));
		break;

	case FLOAT8OID:
		result = jl_box_float64(atof(value));
		break;

	case NUMERICOID:
		/*
		 * numeric can be int, float or selectable precision in pg, so box to
		 * float64 in julia
		 */
		result = jl_box_float64(atof(value));
		break;

	case BOOLOID:
		if (strcmp(value, "t") == 0)
			result = jl_true;
		else
			result = jl_false;
		break;

	default:
		/* return a string representation for everything else */
		result = jl_cstr_to_string(value);
		break;
	}
	return result;
}

/* This function is used to create a new datatype for the named tuple
 * that corresponds to a composite input. We return jl_value_t * instead of
 * jl_datatype_t * because jl_apply_tuple_type_v(types, n) accepts an array of
 * jl_value_t * for the types
 */
jl_value_t *
pg_oid_to_jl_datatype(Oid argtype)
{
	jl_value_t *result;
	switch (argtype)
	{
	case INT2OID:
	case INT4OID:
	/* should be jl_box_int32 but this will be taken care of
	 * along with the rest of the input conversion. For a very little while
	 * let's leave it like this and focus on caching the procedure code
	 * working with just some input types */
		result = jl_int64_type;
		break;
	case INT8OID:
		result = jl_int64_type;
		break;

	case FLOAT4OID:
		result = jl_float32_type;
		break;

	case FLOAT8OID:
		result = jl_float64_type;
		break;

	case NUMERICOID:
	/* numeric can be int, float or selectable precision in pg,
	 * so box to float64 in julia
	 */
		result = jl_float64_type;
		break;

	case BOOLOID:
		result = jl_bool_type;
		break;

	case TEXTOID:
	case VARCHAROID:
		result = jl_string_type;
	default:
	/* return a string representation for everything else */
		result = jl_string_type;
		break;
	}
	return (jl_value_t *) result;
}

/*
 * The index_rm is the index in row-major format (C-indexing).
 * This function converts it to the equivalent col-major representation
 * (Julia-indexing)
 */
int
calculate_cm_offset(int index_rm, int *dims, int ndims)
{
	int *indices = (int *) palloc0(sizeof(int) * ndims);
	int offset = index_rm;
	int col_major_offset = 0;
	int i;

	/*
	 * Get the indices (n1, n2, ... , nd) from the offset.
	 * Since the array is stored in row-major order, the
	 * formula that calculates the offset from the indices is
	 * nd + (Nd * (n_{d-1} + N_{d-1} * (n_{d-2} + N_{d-2} * (...))))
	 */
	for (i = ndims - 1; i >= 0; i--)
	{
		indices[i] = offset % dims[i];
		offset = offset / dims[i];
	}

	/* Now calculate the offset for the col-major representation */
	for (i = ndims - 1; i >= 0; i--)
	{
		col_major_offset = col_major_offset * dims[i] + indices[i];
	}
	pfree(indices);
	return col_major_offset;
}
