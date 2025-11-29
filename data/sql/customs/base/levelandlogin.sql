-- level milníky
CREATE TABLE IF NOT EXISTS `customs`.`level_milestones` (
  `account`     INT UNSIGNED NOT NULL,
  `guid`        INT UNSIGNED NOT NULL,
  `milestone`   TINYINT UNSIGNED NOT NULL,
  `rewarded_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`account`, `milestone`, `guid`),
  KEY `idx_acc_mil` (`account`, `milestone`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;


-- login streak
CREATE TABLE IF NOT EXISTS `customs`.`login_streak` (
  `account`            INT UNSIGNED NOT NULL PRIMARY KEY,
  `last_serial`        INT UNSIGNED NOT NULL,
  `last_reward_serial` INT UNSIGNED NOT NULL,
  `streak_day`         TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `updated_at`         TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
