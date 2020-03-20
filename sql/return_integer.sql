CREATE FUNCTION julia_increment(x INTEGER)
RETURNS INTEGER AS $$
    x + 1
$$ LANGUAGE pljulia;
SELECT julia_increment(4);
DROP FUNCTION julia_increment(x INTEGER);
