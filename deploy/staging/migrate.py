"""Upgrade only the isolated staging database before starting the application."""
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import subprocess

from guard import validate, validate_compose

HERE = Path(__file__).resolve().parent
INIT = HERE.parents[1] / 'sql/init.sql'
SCHEMA_QUERY = """SELECT JSON_OBJECT('table',TABLE_NAME,'column',COLUMN_NAME,
    'type',DATA_TYPE,'nullable',IS_NULLABLE) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME IN
    ('users','tenants','user_token_usage_daily','auth_sessions')
    ORDER BY TABLE_NAME,ORDINAL_POSITION;"""
IDENTITIES = 'SELECT id,tenant_id,username,password_hash,role,status FROM users ORDER BY id;'
USAGE = 'SELECT * FROM user_token_usage_daily ORDER BY id;'


def read_schema(raw):
    return {(row['table'], row['column']): row
            for row in map(json.loads, raw.decode().splitlines())}


def migration_plan(schema):
    statements = []
    for table, column, definition in [
        ('tenants', 'token_limit_daily', 'BIGINT DEFAULT 100000'),
        ('user_token_usage_daily', 'token_limit', 'BIGINT NOT NULL'),
        ('user_token_usage_daily', 'tokens_used', 'BIGINT NOT NULL DEFAULT 0'),
    ]:
        entry = schema.get((table, column))
        if not entry or entry['type'] not in ('int', 'bigint'):
            raise ValueError(f'Unexpected staging schema: {table}.{column}')
        if entry['type'] != 'bigint':
            statements.append(f'ALTER TABLE {table} MODIFY {column} {definition};')
    for column, definition, data_type in [
        ('daily_token_quota', 'BIGINT NULL DEFAULT NULL AFTER password_hash', 'bigint'),
        ('auth_version', 'INT NOT NULL DEFAULT 0', 'int'),
        ('must_change_password', 'BOOLEAN NOT NULL DEFAULT FALSE', 'tinyint'),
    ]:
        entry = schema.get(('users', column))
        if not entry:
            statements.append(f'ALTER TABLE users ADD COLUMN {column} {definition};')
        elif entry['type'] != data_type or (column == 'daily_token_quota' and entry['nullable'] != 'YES'):
            raise ValueError(f'Unexpected staging schema: users.{column}')
    session_columns = {column for table, column in schema if table == 'auth_sessions'}
    if not session_columns:
        ddl = re.search(r'CREATE TABLE IF NOT EXISTS auth_sessions \([\s\S]*?\n\);', INIT.read_text())
        if not ddl:
            raise ValueError('Authoritative auth_sessions definition is missing')
        statements.append(ddl.group())
    elif not {'id', 'user_id', 'auth_version', 'refresh_hash', 'expires_at', 'created_at'} <= session_columns:
        raise ValueError('Incomplete staging auth_sessions table; restore or inspect it before retrying')
    return statements


def upgrade(database, backup_directory):
    plan = migration_plan(read_schema(database.sql(SCHEMA_QUERY)))
    if not plan:
        print('Staging schema already current')
        return False
    identities, usage = database.sql(IDENTITIES), database.sql(USAGE)
    backup_directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    snapshot = backup_directory / f'before-schema-{stamp}.sql'
    with snapshot.open('xb') as output:
        os.chmod(snapshot, 0o600)
        database.dump(output)
    # MySQL DDL commits independently. Recomputing this plan on retry handles
    # an interruption after any individual ADD COLUMN or CREATE TABLE.
    for statement in plan:
        database.sql(statement)
    if migration_plan(read_schema(database.sql(SCHEMA_QUERY))):
        raise RuntimeError('Staging schema upgrade is incomplete')
    if database.sql(IDENTITIES) != identities or database.sql(USAGE) != usage:
        raise RuntimeError('Staging identities or daily usage changed during upgrade')
    print(f'Staging schema upgraded ({len(plan)} operations); identities/usage preserved; backup: {snapshot.name}')
    return True


class ComposeDatabase:
    def __init__(self, root):
        self.compose = ['docker', 'compose', '--env-file', str(root / '.env'),
                        '--file', str(HERE / 'compose.json'), 'exec', '--no-TTY', 'mysql']

    def sql(self, statement):
        return subprocess.run(self.compose + ['sh', '-c',
            'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" exec mysql --user=root --batch --skip-column-names llm_platform'],
            input=statement.encode(), stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True).stdout

    def dump(self, output):
        subprocess.run(self.compose + ['sh', '-c',
            'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" exec mysqldump --user=root --single-transaction '
            '--hex-blob --set-gtid-purged=OFF --no-tablespaces llm_platform'],
            stdout=output, stderr=subprocess.PIPE, check=True)


if __name__ == '__main__':
    try:
        root = validate(os.environ)
        validate_compose(json.loads((HERE / 'compose.json').read_text()))
        os.environ['REPO_ROOT'] = str(HERE.parents[1])
        upgrade(ComposeDatabase(root), root / 'backups')
    except (ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        # Do not print SQL output, identities, password hashes or client stderr.
        raise SystemExit(f'Staging schema preflight failed: {error}') from None
