-- Test SD: per-function private dictionary

-- Test 1: basic persistence across calls
CREATE FUNCTION sd_counter() RETURNS int AS $$
    if !haskey(SD, "n")
        SD["n"] = 0
    end
    SD["n"] += 1
    return SD["n"]
$$ LANGUAGE pljulia;

SELECT sd_counter();
SELECT sd_counter();
SELECT sd_counter();

-- Test 2: SD is private — two functions do not share state
CREATE FUNCTION sd_func_a() RETURNS text AS $$
    if !haskey(SD, "who")
        SD["who"] = "set by func_a"
    end
    return SD["who"]
$$ LANGUAGE pljulia;

CREATE FUNCTION sd_func_b() RETURNS text AS $$
    if !haskey(SD, "who")
        SD["who"] = "set by func_b"
    end
    return SD["who"]
$$ LANGUAGE pljulia;

SELECT sd_func_a();
SELECT sd_func_b();
SELECT sd_func_a();

-- Test 3: SD resets after CREATE OR REPLACE
CREATE FUNCTION sd_reset_test() RETURNS int AS $$
    if !haskey(SD, "n")
        SD["n"] = 0
    end
    SD["n"] += 1
    return SD["n"]
$$ LANGUAGE pljulia;

SELECT sd_reset_test();
SELECT sd_reset_test();

CREATE OR REPLACE FUNCTION sd_reset_test() RETURNS int AS $$
    if !haskey(SD, "n")
        SD["n"] = 0
    end
    SD["n"] += 1
    return SD["n"]
$$ LANGUAGE pljulia;

SELECT sd_reset_test();

-- Cleanup
DROP FUNCTION sd_counter();
DROP FUNCTION sd_func_a();
DROP FUNCTION sd_func_b();
DROP FUNCTION sd_reset_test();