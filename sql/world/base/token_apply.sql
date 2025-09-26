-- Úprava itemu 37711 (Mystery Token)
UPDATE item_template
SET name = 'Mystery Token',
    description = 'Záhadný token… na co asi je?\nA Mysterious token… what might it be for?',
    maxcount = 1000,
    stackable = 1000
WHERE entry = 37711;
