import json
import os
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'deploy/staging'))
import migrate


def schema():
    rows = []
    for table, fields in {
        'tenants': {'token_limit_daily': 'bigint'},
        'user_token_usage_daily': {'token_limit': 'bigint', 'tokens_used': 'bigint'},
        'users': {'daily_token_quota': 'bigint', 'auth_version': 'int', 'must_change_password': 'tinyint'},
        'auth_sessions': dict.fromkeys(['id', 'user_id', 'auth_version', 'refresh_hash', 'expires_at', 'created_at'], 'char'),
    }.items():
        for column, data_type in fields.items():
            rows.append({'table': table, 'column': column, 'type': data_type,
                         'nullable': 'YES' if column == 'daily_token_quota' else 'NO'})
    return migrate.read_schema('\n'.join(map(json.dumps, rows)).encode())


class MigrationPlanTests(unittest.TestCase):
    def test_production_root_is_refused_before_database_access(self):
        result = subprocess.run([sys.executable, str(migrate.HERE / 'migrate.py')],
            env={**os.environ, 'STAGING_ROOT': '/opt/fast-llm-reasoner/staging'},
            capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('dedicated CI staging directory', result.stderr)

    def test_fresh_schema_is_noop(self):
        self.assertEqual(migrate.migration_plan(schema()), [])

    def test_partial_auth_migration_resumes_without_duplicate_columns_or_table(self):
        current = schema()
        del current['users', 'must_change_password']
        plan = migrate.migration_plan(current)
        self.assertEqual(len(plan), 1)
        self.assertIn('ADD COLUMN must_change_password', plan[0])

    def test_unexpected_quota_type_and_partial_session_table_fail_closed(self):
        for mutate in ('quota', 'sessions'):
            current = schema()
            if mutate == 'quota':
                current['users', 'daily_token_quota']['type'] = 'varchar'
            else:
                del current['auth_sessions', 'refresh_hash']
            with self.assertRaises(ValueError):
                migrate.migration_plan(current)

    def test_snapshot_preserves_binary_bytes_and_precedes_ddl(self):
        original = schema()
        del original['users', 'auth_version']
        calls = []

        class Database:
            def sql(self, statement):
                if statement == migrate.SCHEMA_QUERY:
                    return '\n'.join(map(json.dumps, original.values())).encode()
                if statement.startswith('ALTER'):
                    calls.append('ddl')
                    original['users', 'auth_version'] = schema()['users', 'auth_version']
                return b'unchanged\xff'

            def dump(self, output):
                calls.append('backup')
                output.write(b'raw snapshot\xff\x00')

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertTrue(migrate.upgrade(Database(), root))
            self.assertEqual(next(root.glob('*.sql')).read_bytes(), b'raw snapshot\xff\x00')
        self.assertEqual(calls, ['backup', 'ddl'])


if __name__ == '__main__':
    unittest.main()
