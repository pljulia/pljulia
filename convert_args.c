#include "convert_args.h"

jl_value_t * pg_oid_to_julia(Oid argtype, const char * value)
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
