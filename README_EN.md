# mod-treasure  

### 🇨🇿 [Česká verze](README_CS.md)

## Description (EN)
- This module allows you to:

- Display real players without Randombots and Altbots

- Reward for daily login with bonus day

- Reward for every 10 levels (10,20,30,40,50,60,70,80) for the first 10 characters on an account

- Reward every X hours/minutes

- Reward and Claim system via command

### Requirements
Before use, ensure that the database user from WorldDatabaseInfo (default acore) also has access to the new customs schema:

```sql
GRANT ALL PRIVILEGES ON customs.* TO 'acore'@'localhost';
FLUSH PRIVILEGES;
```

### ⚠️ Warning – Reserved IDs
The module modifies the test token in item_template via world/base/token_apply.sql (optional):
Do not import world/backup/token_revert.sql if you don’t want to restore the original test token.
The module uses its own customs database and tables for writing and reading data which it then utilizes.

### Commands
.online
➝ Displays a list of online players

.reward
➝ Displays the status of your rewards (total earned, claimed, and available to claim)

.reward claim
➝ Claims available rewards

.token
➝ Show available tokens

.token deposit 
➝ Deposit available tokens
➝ Usage: .token deposit 6

.token withdraw
➝ Withdraw available tokens
➝ Usage: .token withdraw 6

## License

This module is licensed under the [GNU General Public License v3.0 (GPL-3.0)](LICENSE).

