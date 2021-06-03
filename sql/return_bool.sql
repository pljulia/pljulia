CREATE FUNCTION julia_compare(x INTEGER, y INTEGER)
RETURNS BOOLEAN AS $$
    x > y
$$ LANGUAGE pljulia;
SELECT julia_compare(4,3);
DROP FUNCTION julia_compare(x INTEGER, y INTEGER);
