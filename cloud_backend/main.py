"""FastAPI entry point; run from the project root with uvicorn."""
import logging
from contextlib import asynccontextmanager
from datetime import date
from typing import Literal

from fastapi import Depends, FastAPI, Query
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer
from sqlalchemy import text
from sqlalchemy.exc import SQLAlchemyError

from . import service
from .config import Settings
from .database import Database, records, shots
from .protocol import Batch

logger = logging.getLogger(__name__)
bearer = HTTPBearer(auto_error=False)


class BodyLimit:
    """Bound actual received bytes as well as Content-Length, including chunks."""
    def __init__(self, app, maximum):
        self.app, self.maximum = app, maximum

    async def __call__(self, scope, receive, send):
        if scope['type'] != 'http' or scope['method'] not in ('POST', 'PUT', 'PATCH'):
            return await self.app(scope, receive, send)
        headers = dict(scope['headers'])
        try:
            length = int(headers.get(b'content-length', b'0'))
            if length < 0:
                raise ValueError
        except ValueError:
            return await JSONResponse({'detail': 'Invalid Content-Length'}, status_code=400)(scope, receive, send)
        if length > self.maximum:
            return await JSONResponse({'detail': 'Request body too large'}, status_code=413)(scope, receive, send)
        body = bytearray()
        while True:
            message = await receive()
            if message['type'] == 'http.disconnect':
                return
            chunk = message.get('body', b'')
            if len(body) + len(chunk) > self.maximum:
                return await JSONResponse({'detail': 'Request body too large'}, status_code=413)(scope, receive, send)
            body.extend(chunk)
            if not message.get('more_body', False):
                break
        replayed = False

        async def bounded_receive():
            nonlocal replayed
            if not replayed:
                replayed = True
                return {'type': 'http.request', 'body': bytes(body), 'more_body': False}
            return await receive()
        await self.app(scope, bounded_receive, send)


def credential(value: HTTPAuthorizationCredentials | None = Depends(bearer)):
    if value is None or value.scheme.lower() != 'bearer' or len(value.credentials) > 256:
        raise service.ServiceError(401, 'Bearer credential required')
    return value.credentials


def create_app(settings=None):
    settings = settings or Settings.from_env()
    db = Database(settings.database_url)

    @asynccontextmanager
    async def lifespan(app):
        db.initialize()
        try:
            yield
        finally:
            db.engine.dispose()

    app = FastAPI(title='检测设备数据接收服务', version='0.1.0', lifespan=lifespan)
    app.state.database = db
    app.add_middleware(BodyLimit, maximum=settings.max_body_bytes)

    @app.exception_handler(service.ServiceError)
    async def domain_error(request, exc):
        headers = {'WWW-Authenticate': 'Bearer'} if exc.status == 401 else None
        return JSONResponse({'detail': exc.message}, status_code=exc.status, headers=headers)

    @app.exception_handler(RequestValidationError)
    async def invalid_request(request, exc):
        # Do not echo arbitrary request bodies or credentials into error responses.
        errors = [dict(loc=list(e['loc']), msg=e['msg'], type=e['type']) for e in exc.errors()]
        return JSONResponse({'detail': errors}, status_code=422)

    @app.exception_handler(SQLAlchemyError)
    async def unavailable(request, exc):
        logger.error('Database operation failed: %s', type(exc).__name__)
        return JSONResponse({'detail': 'Storage unavailable; keep records and retry'},
                            status_code=503, headers={'Retry-After': '5'})

    @app.get('/healthz', tags=['health'])
    def health():
        with db.engine.connect() as connection:
            connection.execute(text('SELECT 1'))
        return {'status': 'ok', 'version': '0.1.0'}

    @app.post('/device/v1/batches', tags=['device'])
    def upload(batch: Batch, token: str = Depends(credential)):
        try:
            return service.ingest(db, token, batch)
        except ValueError as exc:
            return JSONResponse({'detail': str(exc)}, status_code=422)

    @app.get('/api/v1/devices', tags=['tenant query'])
    def get_devices(token: str = Depends(credential), limit: int = Query(100, ge=1, le=200),
                    offset: int = Query(0, ge=0, le=1000000)):
        return service.list_devices(db, token, limit, offset)

    @app.get('/api/v1/records', tags=['tenant query'])
    def get_records(token: str = Depends(credential), deviceId: str | None = None,
                    kind: Literal['sensor', 'gps', 'shot'] | None = None,
                    eventDate: date | None = None, limit: int = Query(100, ge=1, le=200),
                    offset: int = Query(0, ge=0, le=1000000)):
        return service.list_entries(db, token, records, limit, offset, deviceId, kind, eventDate)

    @app.get('/api/v1/records/{record_id}', tags=['tenant query'])
    def get_record(record_id: str, token: str = Depends(credential)):
        return service.record_detail(db, token, record_id)

    @app.get('/api/v1/shots', tags=['tenant query'])
    def get_shots(token: str = Depends(credential), deviceId: str | None = None,
                  eventDate: date | None = None, limit: int = Query(100, ge=1, le=200),
                  offset: int = Query(0, ge=0, le=1000000)):
        return service.list_entries(db, token, shots, limit, offset, deviceId, event_date=eventDate)

    @app.get('/api/v1/statistics', tags=['tenant query'])
    def get_statistics(token: str = Depends(credential), deviceId: str | None = None):
        return service.statistics(db, token, deviceId)

    return app


app = create_app()
