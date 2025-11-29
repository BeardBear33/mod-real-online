CREATE TABLE IF NOT EXISTS `customs`.`rewards` (
  `account`     INT UNSIGNED NOT NULL,
  `item`        INT UNSIGNED NOT NULL,
  `entitled`    INT UNSIGNED NOT NULL DEFAULT 0,
  `claimed`     INT UNSIGNED NOT NULL DEFAULT 0,
  `stored`      INT UNSIGNED NOT NULL DEFAULT 0,
  `updated_at`  TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`account`, `item`),
  KEY `idx_rewards_account` (`account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
