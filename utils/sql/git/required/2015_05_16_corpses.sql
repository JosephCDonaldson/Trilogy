alter table character_corpses add column `rez_time` int(11) not null default 0 AFTER `time_of_death`;
alter table character_corpses_backup add column `rez_time` int(11) not null default 0 AFTER `time_of_death`;