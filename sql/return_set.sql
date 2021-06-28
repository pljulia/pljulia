CREATE FUNCTION julia_setof_int()
RETURNS SETOF INTEGER AS $$
    for i in 1:5
        return_next(i)
    end
$$ LANGUAGE pljulia;

CREATE FUNCTION julia_setof_array()
RETURNS SETOF INTEGER[] AS $$
x =[[1,2], [3,4]]
for i in x
    return_next(i)
end
$$ LANGUAGE pljulia;

CREATE TYPE named_value AS (
  name   text,
  value  integer
);

CREATE FUNCTION julia_setof_composite()
RETURNS SETOF named_value AS $$
x = [("John", 1), ("Mary", 2)]
for i in x
    return_next(i)
end
$$ LANGUAGE pljulia;

SELECT julia_setof_int();
SELECT julia_setof_array();
SELECT julia_setof_composite();

DROP FUNCTION julia_setof_array();
DROP FUNCTION julia_setof_int();
DROP FUNCTION julia_setof_composite();
DROP TYPE named_value;