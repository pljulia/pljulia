CREATE FUNCTION julia_sqrt(x INTEGER)
RETURNS DECIMAL AS $$
    sqrt(x)
$$ LANGUAGE pljulia;
SELECT julia_sqrt(8);
DROP FUNCTION julia_sqrt(x INTEGER);
