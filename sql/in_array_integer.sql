CREATE FUNCTION julia_in_array(x INTEGER[])
RETURNS INTEGER AS $$
    1
$$ LANGUAGE pljulia;
SELECT julia_in_array('{1,2,3}'::INTEGER[]);
DROP FUNCTION julia_in_array(x INTEGER[]);
