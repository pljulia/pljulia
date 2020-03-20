CREATE FUNCTION julia_sqrt(x INTEGER)
RETURNS REAL AS $$
    sqrt(x)
$$ LANGUAGE pljulia;
SELECT julia_sqrt(6);
DROP FUNCTION julia_sqrt(x INTEGER);
