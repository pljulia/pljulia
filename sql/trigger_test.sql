CREATE FUNCTION trigfunc_modcount() RETURNS trigger AS $$
if TD_event == "INSERT"
    # args[1] == "modcnt"
    TD_NEW[args[1]] = 0
    return TD_NEW
elseif TD_event == "UPDATE"
    TD_NEW[args[1]] = TD_OLD[args[1]] + 1
    return TD_NEW
else
    return "OK"
end
$$ language pljulia;

CREATE TABLE mytab (num integer, description text, modcnt integer);

CREATE TRIGGER trig_mytab_modcount BEFORE INSERT OR UPDATE ON mytab
    FOR EACH ROW EXECUTE FUNCTION trigfunc_modcount('modcnt');

INSERT INTO mytab (num, description, modcnt) values (1, 'first', 0), (2, 'second', 0), (1, 'first', 1);
update mytab set description = 'first, modified once' where num = 1;
INSERT INTO mytab (num, description, modcnt) values (3, 'third', 3);

SELECT * FROM mytab;

DROP TRIGGER trig_mytab_modcount ON mytab;
DROP TABLE mytab;
DROP FUNCTION trigfunc_modcount;
