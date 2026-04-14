CREATE OR REPLACE FUNCTION test_date() RETURNS date AS $$
return "2026-04-15"
$$ LANGUAGE pljulia;
SELECT test_date();
