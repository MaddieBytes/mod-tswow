CREATE OR REPLACE VIEW `player_classlevelstats` AS
SELECT
  `Class` AS `class`,
  `Level` AS `level`,
  `BaseHP` AS `basehp`,
  `BaseMana` AS `basemana`
FROM `player_class_stats`;

-- AzerothCore stores class stats and race offsets separately. TSWoW's public
-- datascript API expects their combined value in one editable table. The
-- datascript runner validates and folds this table back into AzerothCore after
-- writing it.
CREATE TABLE IF NOT EXISTS `player_levelstats` (
  `race` tinyint unsigned NOT NULL,
  `class` tinyint unsigned NOT NULL,
  `level` tinyint unsigned NOT NULL,
  `str` tinyint unsigned NOT NULL DEFAULT '0',
  `agi` tinyint unsigned NOT NULL DEFAULT '0',
  `sta` tinyint unsigned NOT NULL DEFAULT '0',
  `inte` tinyint unsigned NOT NULL DEFAULT '0',
  `spi` tinyint unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`race`, `class`, `level`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT IGNORE INTO `player_levelstats`
  (`race`, `class`, `level`, `str`, `agi`, `sta`, `inte`, `spi`)
SELECT
  race_stats.`Race`,
  class_stats.`Class`,
  class_stats.`Level`,
  CAST(class_stats.`Strength` AS SIGNED) + race_stats.`Strength`,
  CAST(class_stats.`Agility` AS SIGNED) + race_stats.`Agility`,
  CAST(class_stats.`Stamina` AS SIGNED) + race_stats.`Stamina`,
  CAST(class_stats.`Intellect` AS SIGNED) + race_stats.`Intellect`,
  CAST(class_stats.`Spirit` AS SIGNED) + race_stats.`Spirit`
FROM `player_class_stats` class_stats
CROSS JOIN `player_race_stats` race_stats;

-- TSWoW exposes TrinityCore's four model slots, scale, and immunity masks on
-- creature_template. AzerothCore normalizes those values into related tables.
-- Keep the public datascript shape here; the runner synchronizes it back to the
-- native AzerothCore tables after each build.
SET @tswow_add_creature_template_columns = IF(
  EXISTS(
    SELECT 1 FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'creature_template'
      AND COLUMN_NAME = 'modelid1'
  ),
  'SELECT 1',
  'ALTER TABLE `creature_template`
     ADD COLUMN `modelid1` int unsigned NOT NULL DEFAULT ''0'',
     ADD COLUMN `modelid2` int unsigned NOT NULL DEFAULT ''0'',
     ADD COLUMN `modelid3` int unsigned NOT NULL DEFAULT ''0'',
     ADD COLUMN `modelid4` int unsigned NOT NULL DEFAULT ''0'',
     ADD COLUMN `scale` float NOT NULL DEFAULT ''1'',
     ADD COLUMN `mechanic_immune_mask` bigint unsigned NOT NULL DEFAULT ''0'',
     ADD COLUMN `spell_school_immune_mask` int unsigned NOT NULL DEFAULT ''0'''
);
PREPARE tswow_add_creature_template_columns
  FROM @tswow_add_creature_template_columns;
EXECUTE tswow_add_creature_template_columns;
DEALLOCATE PREPARE tswow_add_creature_template_columns;

UPDATE `creature_template` template
LEFT JOIN `creature_template_model` model0
  ON model0.CreatureID = template.entry AND model0.Idx = 0
LEFT JOIN `creature_template_model` model1
  ON model1.CreatureID = template.entry AND model1.Idx = 1
LEFT JOIN `creature_template_model` model2
  ON model2.CreatureID = template.entry AND model2.Idx = 2
LEFT JOIN `creature_template_model` model3
  ON model3.CreatureID = template.entry AND model3.Idx = 3
LEFT JOIN `creature_immunities` immunities
  ON immunities.ID = template.CreatureImmunitiesId
SET template.modelid1 = COALESCE(model0.CreatureDisplayID, 0),
    template.modelid2 = COALESCE(model1.CreatureDisplayID, 0),
    template.modelid3 = COALESCE(model2.CreatureDisplayID, 0),
    template.modelid4 = COALESCE(model3.CreatureDisplayID, 0),
    template.scale = COALESCE(model0.DisplayScale, 1),
    template.mechanic_immune_mask = COALESCE(immunities.MechanicsMask, 0),
    template.spell_school_immune_mask = COALESCE(immunities.SchoolMask, 0);
