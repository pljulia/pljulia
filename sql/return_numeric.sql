CREATE FUNCTION julia_sqrt(x INTEGER)
RETURNS NUMERIC AS $$
    sqrt(x)
$$ LANGUAGE pljulia;
SELECT julia_sqrt(2);
DROP FUNCTION julia_sqrt(x INTEGER);
