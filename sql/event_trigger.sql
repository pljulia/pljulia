CREATE OR REPLACE FUNCTION jlsnitch() RETURNS event_trigger AS $$
    elog_msg = "You invoked a command tagged: " * TD_tag * " on event " * TD_event 
    elog("INFO", elog_msg)
$$ language pljulia;

CREATE EVENT TRIGGER julia_a_snitch ON ddl_command_start EXECUTE FUNCTION jlsnitch();

create event trigger julia_b_snitch on ddl_command_end
   execute procedure jlsnitch();

create or replace function foobar() returns int language sql as $$select 1;$$;
alter function foobar() cost 77;
drop function foobar();

create table foo();
drop table foo;

drop event trigger julia_a_snitch;
drop event trigger julia_b_snitch;