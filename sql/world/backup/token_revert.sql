-- Vrácení změn itemu 37711 (původní hodnoty)
UPDATE item_template
SET name = 'Currency Token Test Token 1',
    description = 'A test token for the Currency Token system',
    maxcount = 100,
    stackable = 100
WHERE entry = 37711;
