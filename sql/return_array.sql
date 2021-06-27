CREATE FUNCTION int_arr(x integer) RETURNS integer[] AS $$
collect(1:x)
$$ LANGUAGE pljulia;
select int_arr(10);
DROP FUNCTION int_arr(integer);

CREATE FUNCTION float_arr() RETURNS float4[] AS $$
    A = Array{Float64, 2}(undef, 2, 3)
    A .= [10, 20] .+ [1.93 2 3]
    return A
$$ LANGUAGE pljulia;
select float_arr();
DROP FUNCTION float_arr();

CREATE FUNCTION return_input_array(x integer[]) RETURNS integer[] AS $$
return x
$$ LANGUAGE pljulia;
select return_input_array('{{1,2,3}, {4,5,6}}'::INTEGER[]);
DROP FUNCTION return_input_array(integer []);

CREATE FUNCTION return_array_with_nulls() RETURNS integer[] AS $$
y = [1,2,3,nothing,5,6]
$$ LANGUAGE pljulia;
select return_array_with_nulls();
DROP FUNCTION return_array_with_nulls();

CREATE FUNCTION string_1d() RETURNS TEXT[] AS $$
fill("some string", 3)
$$ LANGUAGE pljulia;
select string_1d();
DROP FUNCTION string_1d();

CREATE FUNCTION string_2d() RETURNS TEXT[] AS $$
[i*j for i in "abc", j in "123"]
$$ LANGUAGE pljulia;
select string_2d();
DROP FUNCTION string_2d();

CREATE FUNCTION string_3d() returns text[] AS $$
[i*j*k for i in "abc", j in "123", k in "fn"]
$$ LANGUAGE pljulia;
select string_3d();
DROP FUNCTION  string_3d();

CREATE FUNCTION string_4d() returns text[] AS $$
[i*j*k*l for i in "abc", j in "123", k in "fn", l in "str"]
$$ LANGUAGE pljulia;
select string_4d();
DROP FUNCTION  string_4d();

CREATE TYPE named_value AS (
  name   text,
  value  integer
);

CREATE FUNCTION comp_arr() returns named_value[] AS $$
return [("Name", 7), ("Surname", 8)]
$$ LANGUAGE pljulia;

select comp_arr();

DROP FUNCTION comp_arr();
DROP TYPE named_value;
