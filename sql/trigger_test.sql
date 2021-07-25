CREATE FUNCTION trigfunc_modcount() RETURNS trigger AS $$
if TD_event == "INSERT"
    TD_NEW["modcnt"] = 0
    return TD_NEW
elseif TD_event == "UPDATE"
    TD_NEW["modcnt"] = TD_OLD["modcnt"] + 1
    return TD_NEW
else
    return "OK"
end
$$ language pljulia;

CREATE TABLE mytab (num integer, description text, modcnt integer);
INSERT INTO mytab (num, description, modcnt) values (1, 'first', 0), (2, 'second', 0);

CREATE TRIGGER trig_mytab_modcount BEFORE INSERT OR UPDATE ON mytab
    FOR EACH ROW EXECUTE FUNCTION trigfunc_modcount('modcnt');

update mytab set description = 'first, modified once' where num = 1;
INSERT INTO mytab (num, description, modcnt) values (3, 'third', 3);

SELECT * FROM mytab;

DROP TRIGGER trig_mytab_modcount ON mytab;
DROP TABLE mytab;
DROP FUNCTION trigfunc_modcount;
