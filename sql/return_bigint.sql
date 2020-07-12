CREATE FUNCTION julia_increment(x INTEGER)
RETURNS BIGINT AS $$
    x + 1
$$ LANGUAGE pljulia;
SELECT julia_increment(99999);
DROP FUNCTION julia_increment(x INTEGER);
