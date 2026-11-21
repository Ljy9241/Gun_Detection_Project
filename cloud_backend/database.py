"""Shared relational schema and transaction boundaries for SQLite/PostgreSQL."""
from pathlib import Path

from sqlalchemy import (BigInteger, Boolean, Column, ForeignKey, Index, Integer, JSON,
                        MetaData, String, Table, Text, UniqueConstraint,
                        create_engine, event)
from sqlalchemy.engine import make_url

metadata = MetaData()
tenants = Table('tenants', metadata,
    Column('id', String(64), primary_key=True),
    Column('name', String(128), nullable=False))
readers = Table('tenant_readers', metadata,
    Column('token_hash', String(64), primary_key=True),
    Column('tenant_id', ForeignKey('tenants.id'), nullable=False),
    Column('enabled', Boolean, nullable=False, default=True))
devices = Table('devices', metadata,
    Column('id', String(64), primary_key=True),
    Column('tenant_id', ForeignKey('tenants.id'), nullable=False),
    Column('name', String(128), nullable=False),
    Column('token_hash', String(64), nullable=False, unique=True),
    Column('enabled', Boolean, nullable=False, default=True),
    Column('last_received_at', String(40)))
batches = Table('ingest_batches', metadata,
    Column('device_id', ForeignKey('devices.id'), primary_key=True),
    Column('storage_epoch', String(64), primary_key=True),
    Column('batch_id', String(64), primary_key=True),
    Column('payload_hash', String(64), nullable=False),
    Column('received_at', String(40), nullable=False))
records = Table('records', metadata,
    Column('id', String(32), primary_key=True),
    Column('tenant_id', ForeignKey('tenants.id'), nullable=False),
    Column('device_id', ForeignKey('devices.id'), nullable=False),
    Column('storage_epoch', String(64), nullable=False),
    Column('boot_id', String(16), nullable=False),
    # Text preserves the complete unsigned 64-bit range on both databases.
    Column('sequence', String(20), nullable=False),
    Column('kind', String(8), nullable=False),
    Column('shot_id', BigInteger, nullable=False),
    Column('uptime_ms', String(20), nullable=False),
    Column('occurred_at', String(40)),
    Column('event_date', String(10)),
    Column('received_at', String(40), nullable=False),
    Column('payload_hash', String(64), nullable=False),
    Column('payload', JSON, nullable=False),
    UniqueConstraint('device_id', 'storage_epoch', 'boot_id', 'sequence', name='record_identity'))
Index('records_tenant_received', records.c.tenant_id, records.c.received_at, records.c.id)
Index('records_device_time', records.c.tenant_id, records.c.device_id, records.c.occurred_at)
shots = Table('shot_events', metadata,
    Column('id', String(32), primary_key=True),
    Column('tenant_id', ForeignKey('tenants.id'), nullable=False),
    Column('device_id', ForeignKey('devices.id'), nullable=False),
    Column('storage_epoch', String(64), nullable=False),
    Column('boot_id', String(16), nullable=False),
    Column('shot_id', BigInteger, nullable=False),
    Column('record_id', ForeignKey('records.id'), nullable=False),
    Column('payload_hash', String(64), nullable=False),
    Column('occurred_at', String(40)),
    Column('event_date', String(10)),
    Column('received_at', String(40), nullable=False),
    Column('payload', JSON, nullable=False),
    UniqueConstraint('device_id', 'storage_epoch', 'boot_id', 'shot_id', name='shot_identity'))
Index('shots_tenant_time', shots.c.tenant_id, shots.c.occurred_at)
outbox = Table('outbox_jobs', metadata,
    Column('id', String(32), primary_key=True),
    Column('tenant_id', ForeignKey('tenants.id'), nullable=False),
    Column('record_id', ForeignKey('records.id'), nullable=False, unique=True),
    Column('job_type', String(32), nullable=False),
    Column('created_at', String(40), nullable=False))
schema_versions = Table('schema_versions', metadata,
    Column('version', Integer, primary_key=True), Column('note', Text, nullable=False))


class Database:
    def __init__(self, url):
        parsed = make_url(url)
        if parsed.get_backend_name() not in ('sqlite', 'postgresql'):
            raise ValueError('Only SQLite and PostgreSQL are supported')
        options = {'pool_pre_ping': True}
        if parsed.get_backend_name() == 'sqlite':
            if not parsed.database or parsed.database == ':memory:':
                raise ValueError('Use a file-backed SQLite database for durable reception')
            Path(parsed.database).resolve().parent.mkdir(parents=True, exist_ok=True)
            options['connect_args'] = {'check_same_thread': False, 'timeout': 15}
        self.engine = create_engine(url, **options)
        if self.engine.dialect.name == 'sqlite':
            @event.listens_for(self.engine, 'connect')
            def sqlite_connection(connection, _):
                connection.isolation_level = None
                cursor = connection.cursor()
                cursor.execute('PRAGMA foreign_keys=ON')
                cursor.execute('PRAGMA journal_mode=WAL')
                cursor.execute('PRAGMA synchronous=FULL')
                cursor.close()

            @event.listens_for(self.engine, 'begin')
            def sqlite_begin(connection):
                # A local write transaction must not race after reading a receipt.
                connection.exec_driver_sql('BEGIN IMMEDIATE')

    def initialize(self):
        metadata.create_all(self.engine)
        with self.engine.begin() as connection:
            versions = list(connection.execute(schema_versions.select()).scalars())
            if not versions:
                connection.execute(schema_versions.insert().values(version=1, note='Initial receiver schema'))
            elif versions != [1]:
                raise RuntimeError('Unsupported database schema version')
