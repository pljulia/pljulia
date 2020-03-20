CREATE FUNCTION julia_sqrt(x INTEGER)
RETURNS DOUBLE PRECISION AS $$
    sqrt(x)
$$ LANGUAGE pljulia;
SELECT julia_sqrt(5);
DROP FUNCTION julia_sqrt(x INTEGER);
