"""Authenticated ingestion. Returning an ACK always follows transaction commit."""
import hashlib
import re
import secrets
from datetime import datetime, timezone
from uuid import uuid4

from sqlalchemy import func, select

from .database import batches, devices, outbox, readers, records, shots, tenants
from .protocol import IDENTIFIER, digest, normalize, canonical_payload


class ServiceError(Exception):
    def __init__(self, status, message):
        self.status, self.message = status, message


def token_hash(token):
    return hashlib.sha256(token.encode('utf-8')).hexdigest()


def now():
    return datetime.now(timezone.utc).isoformat()


def provision_tenant(db, identifier, name):
    validate_registration(identifier, name)
    token = secrets.token_urlsafe(32)
    with db.engine.begin() as connection:
        connection.execute(tenants.insert().values(id=identifier, name=name))
        connection.execute(readers.insert().values(tenant_id=identifier, token_hash=token_hash(token)))
    return token


def provision_device(db, tenant_id, identifier, name):
    validate_registration(identifier, name)
    token = secrets.token_urlsafe(32)
    with db.engine.begin() as connection:
        if not connection.execute(select(tenants.c.id).where(tenants.c.id == tenant_id)).first():
            raise ValueError('Tenant does not exist')
        connection.execute(devices.insert().values(id=identifier, name=name,
            tenant_id=tenant_id, token_hash=token_hash(token)))
    return token


def validate_registration(identifier, name):
    if not re.fullmatch(IDENTIFIER, identifier) or not name.strip() or len(name) > 128:
        raise ValueError('Invalid identifier or name')


def authenticated_device(connection, token, lock=False):
    query = select(devices).where(devices.c.token_hash == token_hash(token), devices.c.enabled.is_(True))
    if lock:
        query = query.with_for_update()
    device = connection.execute(query).mappings().first()
    if not device:
        raise ServiceError(401, 'Invalid or disabled device credential')
    return device


def authenticated_tenant(connection, token):
    tenant = connection.execute(select(readers.c.tenant_id).where(
        readers.c.token_hash == token_hash(token), readers.c.enabled.is_(True))).scalar_one_or_none()
    if not tenant:
        raise ServiceError(401, 'Invalid or disabled tenant query credential')
    return tenant


def shot_display_payload(payload):
    keys = ('shotId', 'timestamp', 'uptimeMs', 'gpsValid', 'latitude', 'longitude',
            'original_timestamp', 'timestamp_source', 'time_anchor')
    return {key: payload[key] for key in keys if key in payload}


def ingest(db, token, batch):
    normalized = normalize(batch)
    batch_hash = digest([canonical_payload(item['payload']) for item in normalized])
    inserted, duplicates = 0, 0
    received_at = now()
    with db.engine.begin() as connection:
        # PostgreSQL serializes uploads per device; SQLite uses BEGIN IMMEDIATE.
        device = authenticated_device(connection, token, lock=True)
        if device['id'] != batch.deviceId:
            raise ServiceError(403, 'Credential does not belong to this device')
        batch_identity = dict(device_id=device['id'], storage_epoch=batch.storageEpoch, batch_id=batch.batchId)
        previous = connection.execute(select(batches).filter_by(**batch_identity)).mappings().first()
        if previous and previous['payload_hash'] != batch_hash:
            raise ServiceError(409, 'batchId was already used for different content')
        for item in normalized:
            payload = item['payload']
            identity = dict(device_id=device['id'], storage_epoch=batch.storageEpoch,
                            boot_id=payload['bootId'], sequence=payload['sequence'])
            existing = connection.execute(select(records.c.id, records.c.payload_hash, records.c.occurred_at).filter_by(
                **identity)).mappings().first()
            if existing:
                if existing['payload_hash'] != item['payload_hash']:
                    raise ServiceError(409, 'Record identity was already used for different content')
                # Enrich a legacy T+ record without changing its original hash,
                # receipt, identity or event count. A stored calendar time is frozen.
                if existing['occurred_at'] is None and payload.get('timestamp_source') == 'inferred':
                    dates = dict(occurred_at=item['occurred_at'], event_date=payload['timestamp'][:10])
                    connection.execute(records.update().where(records.c.id == existing['id']).values(
                        **dates, payload=payload))
                    if payload['shotId']:
                        connection.execute(shots.update().where(shots.c.record_id == existing['id']).values(
                            **dates, payload=shot_display_payload(payload)))
                duplicates += 1
                continue
            record_id = uuid4().hex
            event_date = payload['timestamp'][:10] if item['occurred_at'] else None
            connection.execute(records.insert().values(**identity, id=record_id,
                tenant_id=device['tenant_id'], kind=item['kind'], shot_id=payload['shotId'],
                uptime_ms=payload['uptimeMs'], occurred_at=item['occurred_at'], event_date=event_date,
                received_at=received_at, payload_hash=item['payload_hash'], payload=payload))
            if payload['shotId']:
                shot_identity = dict(device_id=device['id'], storage_epoch=batch.storageEpoch,
                                     boot_id=payload['bootId'], shot_id=payload['shotId'])
                shot_payload = {key: payload[key] for key in (
                    'shotId', 'timestamp', 'uptimeMs', 'gpsValid', 'latitude', 'longitude')}
                shot_hash = digest({key: canonical_payload(payload)[key] for key in shot_payload})
                shot_payload = shot_display_payload(payload)
                existing_shot = connection.execute(select(shots.c.payload_hash).filter_by(
                    **shot_identity)).scalar_one_or_none()
                if existing_shot and existing_shot != shot_hash:
                    raise ServiceError(409, 'Shot identity was already used for different content')
                if not existing_shot:
                    connection.execute(shots.insert().values(**shot_identity, id=uuid4().hex,
                        tenant_id=device['tenant_id'], record_id=record_id, payload_hash=shot_hash,
                        occurred_at=item['occurred_at'], event_date=event_date,
                        received_at=received_at, payload=shot_payload))
            connection.execute(outbox.insert().values(id=uuid4().hex, tenant_id=device['tenant_id'],
                record_id=record_id, job_type='archive_record', created_at=received_at))
            inserted += 1
        if not previous:
            connection.execute(batches.insert().values(**batch_identity, payload_hash=batch_hash,
                                                       received_at=received_at))
        connection.execute(devices.update().where(devices.c.id == device['id']).values(
            last_received_at=received_at))
    # This code is reached only if the context manager's COMMIT succeeded.
    return dict(committed=True, deviceId=device['id'], storageEpoch=batch.storageEpoch,
        batchId=batch.batchId, acceptedCount=len(normalized), insertedCount=inserted,
        duplicateCount=duplicates, records=[dict(bootId=item['payload']['bootId'],
        sequence=item['payload']['sequence']) for item in normalized])


def require_device_scope(connection, tenant_id, device_id):
    if device_id is not None and not connection.execute(select(devices.c.id).where(
            devices.c.id == device_id, devices.c.tenant_id == tenant_id)).first():
        raise ServiceError(404, 'Device not found')


def list_devices(db, token, limit, offset):
    with db.engine.begin() as connection:
        tenant = authenticated_tenant(connection, token)
        predicate = devices.c.tenant_id == tenant
        total = connection.execute(select(func.count()).select_from(devices).where(predicate)).scalar_one()
        query = select(devices.c.id, devices.c.name, devices.c.enabled, devices.c.last_received_at).where(
            predicate).order_by(devices.c.id).limit(limit).offset(offset)
        return dict(total=total, items=[dict(row) for row in connection.execute(query).mappings()])


def list_entries(db, token, table, limit, offset, device_id=None, kind=None, event_date=None):
    with db.engine.begin() as connection:
        tenant = authenticated_tenant(connection, token)
        require_device_scope(connection, tenant, device_id)
        conditions = [table.c.tenant_id == tenant]
        if device_id:
            conditions.append(table.c.device_id == device_id)
        if kind:
            conditions.append(table.c.kind == kind)
        if event_date:
            conditions.append(table.c.event_date == event_date.isoformat())
        total = connection.execute(select(func.count()).select_from(table).where(*conditions)).scalar_one()
        query = select(table).where(*conditions).order_by(table.c.received_at, table.c.id).limit(limit).offset(offset)
        items = []
        for row in connection.execute(query).mappings():
            entry = dict(row)
            entry.pop('payload_hash')
            items.append(entry)
        return dict(total=total, items=items)


def record_detail(db, token, record_id):
    with db.engine.begin() as connection:
        tenant = authenticated_tenant(connection, token)
        row = connection.execute(select(records).where(records.c.id == record_id,
            records.c.tenant_id == tenant)).mappings().first()
        if not row:
            raise ServiceError(404, 'Record not found')
        result = dict(row)
        result.pop('payload_hash')
        return result


def statistics(db, token, device_id=None):
    with db.engine.begin() as connection:
        tenant = authenticated_tenant(connection, token)
        require_device_scope(connection, tenant, device_id)
        record_filters = [records.c.tenant_id == tenant]
        shot_filters = [shots.c.tenant_id == tenant]
        if device_id:
            record_filters.append(records.c.device_id == device_id)
            shot_filters.append(shots.c.device_id == device_id)
        count = connection.execute(select(func.count()).select_from(records).where(*record_filters)).scalar_one()
        rows = connection.execute(select(shots.c.event_date, func.count().label('count')).where(
            *shot_filters).group_by(shots.c.event_date).order_by(shots.c.event_date)).mappings().all()
        return dict(totalRecords=count, totalShots=sum(row['count'] for row in rows),
            shotsByDate=[dict(date=row['event_date'], count=row['count']) for row in rows])
