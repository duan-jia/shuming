"""Verify actual MySQL upgrades in temporary databases inside a CI test container."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'deploy/staging'))
import migrate

LEGACY = """
CREATE TABLE tenants (id INT PRIMARY KEY, token_limit_daily INT DEFAULT 100000);
CREATE TABLE users (id INT PRIMARY KEY, tenant_id INT NOT NULL, username VARCHAR(64),
    password_hash VARCHAR(256), role VARCHAR(16), status VARCHAR(16));
CREATE TABLE user_token_usage_daily (id INT PRIMARY KEY, tenant_id INT, user_id INT,
    usage_date DATE, token_limit INT NOT NULL, tokens_used INT NOT NULL DEFAULT 0);
INSERT INTO tenants VALUES (1,2147483647);
INSERT INTO users VALUES (1,1,'migration-fixture','unchanged-fixture-hash','user','active');
INSERT INTO user_token_usage_daily VALUES (1,1,1,CURDATE(),2147483647,96713);
"""


class Database:
    def __init__(self, container, name):
        self.container, self.name = container, name

    def sql(self, statement, database=None):
        return subprocess.run(['docker', 'exec', '--interactive', self.container, 'sh', '-c',
            'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" exec mysql --user=root --batch --skip-column-names "$1"',
            '_', self.name if database is None else database], input=statement.encode(),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True).stdout

    def dump(self, output):
        subprocess.run(['docker', 'exec', self.container, 'sh', '-c',
            'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" exec mysqldump --user=root --single-transaction '
            '--hex-blob --set-gtid-purged=OFF --no-tablespaces "$1"', '_', self.name],
            stdout=output, stderr=subprocess.PIPE, check=True)


def verify(container):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', container):
        raise ValueError('A CI MySQL container name or ID is required')
    for case in ('fresh', 'legacy', 'interrupted'):
        name = 'shuming_migration_test_' + uuid.uuid4().hex
        database = Database(container, name)
        database.sql(f'CREATE DATABASE `{name}`;', database='mysql')
        try:
            if case == 'fresh':
                database.sql((ROOT / 'sql/init.sql').read_text().replace('llm_platform', name))
            else:
                database.sql(LEGACY)
                if case == 'interrupted':
                    database.sql('ALTER TABLE users ADD COLUMN auth_version INT NOT NULL DEFAULT 0;')
                    ddl = re.search(r'CREATE TABLE IF NOT EXISTS auth_sessions \([\s\S]*?\n\);',
                                    (ROOT / 'sql/init.sql').read_text()).group()
                    database.sql(ddl)
            identities, usage = database.sql(migrate.IDENTITIES), database.sql(migrate.USAGE)
            with tempfile.TemporaryDirectory() as directory:
                backups = Path(directory) / 'backups'
                changed = migrate.upgrade(database, backups)
                assert changed == (case != 'fresh')
                assert not migrate.upgrade(database, backups), 'Repeated upgrades must be a no-op'
                assert database.sql(migrate.IDENTITIES) == identities
                assert database.sql(migrate.USAGE) == usage
                if changed:
                    snapshots = list(backups.glob('*.sql'))
                    assert len(snapshots) == 1 and snapshots[0].stat().st_size > 0
                    assert snapshots[0].stat().st_mode & 0o777 == 0o600
                    database.sql('UPDATE tenants SET token_limit_daily=3000000000; '
                                 'UPDATE users SET daily_token_quota=4000000000;')
                    assert database.sql('SELECT tokens_used FROM user_token_usage_daily;').strip() == b'96713'
            print(f'PASS: {case} schema, idempotence, preserved identities/usage and BIGINT limits')
        finally:
            # The name was generated here and CREATE DATABASE succeeded above.
            database.sql(f'DROP DATABASE `{name}`;', database='mysql')


if __name__ == '__main__':
    verify(os.environ['MYSQL_TEST_CONTAINER'])
