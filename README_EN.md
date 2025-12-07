# mod-real-online

### 🇨🇿 [Česká verze](README_CS.md)

## Description (EN)
This module allows you to:  
- Display real players without Randombots and Altbots  
- Reward for daily login with bonus day  
- Reward for every 10 levels (10,20,30,40,50,60,70,80) for the first 10 characters on an account
- Reward every X hours/minutes  
- Reward and Claim system via command  

### Installation / Requirements
The module includes an autoupdater, so there’s no need to manually import any .sql files.  
For the autoupdater to function correctly, it is necessary to ensure that the database user from `(WorldDatabaseInfo) – "127.0.0.1;3306;acore;acore;acore_world"`  
has permissions for the new `customs` database as well:

```
GRANT CREATE ON *.* TO 'acore'@'127.0.0.1';
GRANT ALL PRIVILEGES ON customs.* TO 'acore'@'127.0.0.1';
FLUSH PRIVILEGES;
```  

**Optional:**
- Add this line to worldserver.conf:  
  Logger.gv.customs=3,Console Server

##

### ⚠️ Warning – Reserved IDs
The module modifies the test version of the token in item_template. If you are not using item ID 37711 for any other module, you don’t need to do anything.

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

