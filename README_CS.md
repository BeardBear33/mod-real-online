# mod-real-online

### 🇬🇧 [English version](README_EN.md)

## Popis (CZ)  
Tento modul umožňuje:  
- Zobrazit skutečné hráče bez Randombotů a Altbotů  
- Odměnu za denní login s bonusovým dnem (vázáno na account)
- Odměnu za každých 10 levelů (10,20,30,40,50,60,70,80) pro prvních 10 postav na accountu (ignoruje pouze randomboty)
- Odměnu každých X hodin/minut (ignoruje randomboty i altboty)
- Reward a Claim systém přes příkaz.

### Požadavky  
Před použitím je nutné zajistit, aby uživatel databáze z `WorldDatabaseInfo` (standardně `acore`) měl práva i na novou databázi `customs`:  

```sql
GRANT ALL PRIVILEGES ON customs.* TO 'acore'@'localhost';
FLUSH PRIVILEGES;
```

### ⚠️ Upozornění
Modul používá upravuje testovací token v item_template .sql z world/base/token_apply.sql je volitelné:
Neimportovat .sql z world/backup/token_revert.sql pokud nechcete vrátit původní testovací token
Modul používá vlastní DB customs a tabulky pro zápis a čtení dat které dále používá.

### Příkazy
.online
➝ Zobrazí seznam online hráčů

.reward
➝ Zobrazí stav vašich odměn (celkem získáno,vyzvednuto a k vyzvednutí)

.reward claim
➝ Vyzvedne dostupné odměny

## License

This module is licensed under the [GNU General Public License v2.0 (GPL-2.0)](LICENSE).
