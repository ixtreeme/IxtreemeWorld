USE ixtreemeworld;

-- Replace REPLACE_ME below with an Argon2id password hash and run this script.

INSERT INTO accounts (username, password_hash, email, status)
VALUES (
    'testuser',
    'REPLACE_ME',
    'test@example.com',
    'active'
);

SET @aid = LAST_INSERT_ID();

INSERT INTO characters (account_id, slot, name, level, class_id, pos_x, pos_y, map_id)
VALUES
    (@aid, 0, 'Hero', 1, 0, 0, 0, 0),
    (@aid, 1, 'Mage', 1, 1, 0, 0, 0);
