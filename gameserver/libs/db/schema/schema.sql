-- IxtreemeWorld initial database schema
-- Charset: utf8mb4 (modern Unicode tamogatas)

CREATE DATABASE IF NOT EXISTS ixtreemeworld
    CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

USE ixtreemeworld;

-- =====================================================
-- accounts
-- =====================================================
CREATE TABLE IF NOT EXISTS accounts (
    id              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    username        VARCHAR(32) NOT NULL,
    password_hash   VARCHAR(128) NOT NULL,
    email           VARCHAR(128) NOT NULL DEFAULT '',
    status          ENUM('active','banned') NOT NULL DEFAULT 'active',
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_login_at   DATETIME DEFAULT NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uk_accounts_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- =====================================================
-- characters
-- =====================================================
CREATE TABLE IF NOT EXISTS characters (
    id              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    account_id      BIGINT UNSIGNED NOT NULL,
    slot            TINYINT UNSIGNED NOT NULL,
    name            VARCHAR(32) NOT NULL,
    level           INT UNSIGNED NOT NULL DEFAULT 1,
    experience      BIGINT UNSIGNED NOT NULL DEFAULT 0,
    class_id        SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    appearance      SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    pos_x           INT NOT NULL DEFAULT 0,
    pos_y           INT NOT NULL DEFAULT 0,
    map_id          SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_played_at  DATETIME DEFAULT NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uk_characters_name (name),
    UNIQUE KEY uk_characters_account_slot (account_id, slot),
    KEY idx_characters_account (account_id),
    CONSTRAINT fk_characters_account
        FOREIGN KEY (account_id) REFERENCES accounts(id)
        ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- =====================================================
-- handoff_tokens
-- =====================================================
CREATE TABLE IF NOT EXISTS handoff_tokens (
    token_hash    CHAR(64) NOT NULL,
    account_id    BIGINT UNSIGNED NOT NULL,
    character_id  BIGINT UNSIGNED NOT NULL,
    game_server   VARCHAR(64) NOT NULL,
    issued_at     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at    DATETIME NOT NULL,
    consumed      TINYINT(1) NOT NULL DEFAULT 0,
    PRIMARY KEY (token_hash),
    KEY idx_handoff_account (account_id),
    CONSTRAINT fk_handoff_account
        FOREIGN KEY (account_id) REFERENCES accounts(id)
        ON DELETE CASCADE,
    CONSTRAINT fk_handoff_character
        FOREIGN KEY (character_id) REFERENCES characters(id)
        ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
