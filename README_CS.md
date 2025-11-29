# mod-real-online

### 🇬🇧 [English version](README_EN.md)

## Popis (CZ)  
Tento modul umožňuje:  
- Zobrazit skutečné hráče bez Randombotů a Altbotů  
- Odměnu za denní login s bonusovým dnem (vázáno na account)
- Odměnu za každých 10 levelů (10,20,30,40,50,60,70,80) pro prvních 10 postav na accountu (ignoruje pouze randomboty)
- Odměnu každých X hodin/minut (ignoruje randomboty i altboty)
- Reward a Claim systém přes příkaz.

### Instalace / Požadavky  
Modul obsahuje autoupdater tudíž není potřeba ručně importovat .sql  
Pro správnou funkčnost autoupdateru je nutné zajistit, aby uživatel databáze z `(WorldDatabaseInfo) – "127.0.0.1;3306;acore;acore;acore_world"`  
měl práva i na novou databázi customs:

```
GRANT CREATE ON *.* TO 'acore'@'127.0.0.1';
GRANT ALL PRIVILEGES ON customs.* TO 'acore'@'127.0.0.1';
FLUSH PRIVILEGES;
```  

### ⚠️ Upozornění
Modul upravuje testovací verzi tokenu v item_template pokud nevyužíváte pro žádný jiný modul itemid 37711 není potřeba nic řešit.

### Příkazy
.online
➝ Zobrazí seznam online hráčů

.reward
➝ Zobrazí stav vašich odměn (celkem získáno,vyzvednuto a k vyzvednutí)

.reward claim
➝ Vyzvedne dostupné odměny

.token
➝ zobrazí dostupné odměny

.token deposit 
➝ Uskladní dostupné tokeny
➝ Použití: .token deposit 6

.token withdraw
➝ Vyzvedne dostupné tokeny
➝ Použití: .token withdraw 6


