CREATE OR REPLACE FUNCTION test_timestamp() RETURNS timestamp AS $$
return "2026-04-15 12:30:45"
$$ LANGUAGE pljulia;
SELECT test_timestamp();
