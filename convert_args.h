#include <julia.h>
#include <postgres.h>
#include <catalog/pg_type.h>
#include <utils/syscache.h>
#include <utils/array.h>
#include <utils/lsyscache.h>

jl_value_t * pg_oid_to_jl_value(Oid argtype, const char * value);
jl_value_t * pg_oid_to_jl_datatype(Oid argtype);
int calculate_cm_offset(int index_rm, int *dims, int ndims);

