USE ixtreemeworld;

-- Generate the hash with: db_test --make-hash "test123"
-- Then replace REPLACE_ME below with the output and run this script.

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
